//
//  BlueToolFixup.cpp
//  BrcmPatchRAM
//
//  Created by Dhinak G on 6/11/21.
//


#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_user.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_version.hpp>
#include <Headers/kern_devinfo.hpp>


#define MODULE_SHORT "btlfx"


class EXPORT BlueToolFixup : public IOService {
    OSDeclareDefaultStructors(BlueToolFixup)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(BlueToolFixup, IOService)


IOService *BlueToolFixup::probe(IOService *provider, SInt32 *score) {
    return ADDPR(startSuccess) ? IOService::probe(provider, score) : nullptr;
}

bool BlueToolFixup::start(IOService *provider) {
    if (!IOService::start(provider)) {
        SYSLOG("init", "failed to start the parent");
        return false;
    }
    setProperty("VersionInfo", kextVersion);
    registerService();
    
    return true;
}


#pragma mark - Patches

static const uint8_t kSkipUpdateFilePathOriginal[] = "/etc/bluetool/SkipBluetoothAutomaticFirmwareUpdate";
static const uint8_t kSkipUpdateFilePathPatched[]  = "/System/Library/CoreServices/boot.efi";


// Workaround 12.4 Beta 3+ bug where macOS may detect the Bluetooth chipset twice
// Once as internal, and second as an external dongle:
// 'ERROR -- Third Party Dongle has the same address as the internal module'
// Mainly applicable for BCM2046 and BCM2070 chipsets (BT2.1)
static const uint8_t kSkipAddressCheckOriginal[] =
{
    0x48, 0x89, 0xF3,             // mov    rbx, rsi
    0xE8, 0xE3, 0xF3, 0xFE, 0xFF, // call   sub_1000c5bc6
    0x85, 0xC0,                   // test   eax, eax
    0x74, 0x1D,                   // je
};

static const uint8_t kSkipAddressCheckPatched[] =
{
    0x48, 0x89, 0xF3,             // mov        rbx, rsi
    0xE8, 0xE3, 0xF3, 0xFE, 0xFF, // call       sub_1000c5bc6
    0x85, 0xC0,                   // test       eax, eax
    0x72, 0x1D,                   // jb short
};

static const uint8_t kVendorCheckOriginal[] =
{
    0x81, 0xFA,              // cmp edx
    0x5C, 0x0A, 0x00, 0x00,  // Vendor BRCM,
    0x74                     // jnz short
};

static const uint8_t kVendorCheckPatched[] =
{
    0x81, 0xFA,              // cmp edx
    0x5C, 0x0A, 0x00, 0x00,  // Vendor BRCM,
    0xEB                     // jmp short
};

// Workaround for bugged chipset range check that
// doesn't consider 0 "THIRD_PARTY_DONGLE" as valid.
// This patch allows bluetooth to turn back on after the first power cycle.
// See https://github.com/acidanthera/BrcmPatchRAM/pull/18 for more details.
static const uint8_t kBadChipsetCheckOriginal[] =
{
    0x81, 0xF9,              // cmp ecx
    0xCF, 0x07, 0x00, 0x00,  // int 1999
    0x72                     // jb short
};

static const uint8_t kBadChipsetCheckPatched[] =
{
    0x81, 0xF9,              // cmp ecx
    0xCF, 0x07, 0x00, 0x00,  // int 1999
    0xEB                     // jmp short
};

static bool shouldPatchBoardId = false;
static bool shouldPatchAddress = false;

static const size_t kBoardIdSize = sizeof("Mac-F22586C8");

static const char boardIdsWithUSBBluetooth[][kBoardIdSize] = {
    "Mac-F22586C8"
};

static mach_vm_address_t orig_cs_validate {};

#pragma mark - Kernel patching code

static inline void searchAndPatch(const void *haystack, size_t haystackSize, const char *path, const void *needle, size_t findSize, const void *patch, size_t replaceSize) {
    if (KernelPatcher::findAndReplace(const_cast<void *>(haystack), haystackSize, needle, findSize, patch, replaceSize))
        DBGLOG(MODULE_SHORT, "found string to patch at %s!", path);
}

template <size_t findSize, size_t replaceSize, typename T>
static inline void searchAndPatch(const void *haystack, size_t haystackSize, const char *path, const T (&needle)[findSize], const T (&patch)[replaceSize]) {
    searchAndPatch(haystack, haystackSize, path, needle, findSize * sizeof(T), patch, replaceSize * sizeof(T));
}


#pragma mark - Patched functions

static void patched_cs_validate_page(vnode_t vp, memory_object_t pager, memory_object_offset_t page_offset, const void *data, int *validated_p, int *tainted_p, int *nx_p) {
    char path[PATH_MAX];
    int pathlen = PATH_MAX;
    FunctionCast(patched_cs_validate_page, orig_cs_validate)(vp, pager, page_offset, data, validated_p, tainted_p, nx_p);
    static constexpr size_t dirLength = sizeof("/usr/sbin/")-1;
    if (vn_getpath(vp, path, &pathlen) == 0 && UNLIKELY(strncmp(path, "/usr/sbin/", dirLength) == 0)) {
        if (strcmp(path + dirLength, "BlueTool") == 0) {
            searchAndPatch(data, PAGE_SIZE, path, kSkipUpdateFilePathOriginal, kSkipUpdateFilePathPatched);
            if (shouldPatchBoardId)
                searchAndPatch(data, PAGE_SIZE, path, boardIdsWithUSBBluetooth[0], kBoardIdSize, BaseDeviceInfo::get().boardIdentifier, kBoardIdSize);
        }
        else if (strcmp(path + dirLength, "bluetoothd") == 0) {
            searchAndPatch(data, PAGE_SIZE, path, kVendorCheckOriginal, kVendorCheckPatched);
            searchAndPatch(data, PAGE_SIZE, path, kBadChipsetCheckOriginal, kBadChipsetCheckPatched);
            if (shouldPatchBoardId)
                searchAndPatch(data, PAGE_SIZE, path, boardIdsWithUSBBluetooth[0], kBoardIdSize, BaseDeviceInfo::get().boardIdentifier, kBoardIdSize);
            if (shouldPatchAddress)
                searchAndPatch(data, PAGE_SIZE, path, kSkipAddressCheckOriginal, kSkipAddressCheckPatched);
        }
    }
}


#pragma mark - Patches on start/stop

static void pluginStart() {
    SYSLOG(MODULE_SHORT, "start");
    // There is no point in routing cs_validate_range, because this kext should only be running on Monterey+
    if (getKernelVersion() >= KernelVersion::Monterey) {
        lilu.onPatcherLoadForce([](void *user, KernelPatcher &patcher) {
            auto boardId = BaseDeviceInfo::get().boardIdentifier;
            shouldPatchBoardId = strlen(boardId) + 1 == kBoardIdSize;
            if (shouldPatchBoardId)
                for (size_t i = 0; i < arrsize(boardIdsWithUSBBluetooth); i++)
                    if (strcmp(boardIdsWithUSBBluetooth[i], boardId) == 0) {
                        shouldPatchBoardId = false;
                        break;
                    }
            if ((getKernelVersion() == KernelVersion::Monterey && getKernelMinorVersion() >= 5) || getKernelVersion() > KernelVersion::Monterey)
                // 12.4 Beta 3+, XNU 21.5
                shouldPatchAddress = checkKernelArgument("-btlfxallowanyaddr");
            KernelPatcher::RouteRequest csRoute = KernelPatcher::RouteRequest("_cs_validate_page", patched_cs_validate_page, orig_cs_validate);
            if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &csRoute, 1))
                SYSLOG(MODULE_SHORT, "failed to route cs validation pages");
        });
    }
}

// Boot args.
static const char *bootargOff[] {
    "-btlfxoff"
};
static const char *bootargDebug[] {
    "-btlfxdbg"
};
static const char *bootargBeta[] {
    "-btlfxbeta"
};

// Plugin configuration.
PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Monterey,
    KernelVersion::Monterey,
    pluginStart
};
