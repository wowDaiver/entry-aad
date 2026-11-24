// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "idrive_coding.hpp"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <string>

#include "../platform/car_brand.hpp"
#include "../platform/car_series.hpp"
#include "../platform/controller_layout.hpp"
#include "../platform/driver_side.hpp"
#include "../platform/service_contract.hpp"
#include "app_log.hpp"

namespace evo {
namespace app {

namespace {

// HmiMain3D's exported template instances (nm -D: weak, defined). The usage object is one pointer-sized value,
// returned in r0 (read in pushbuttonToKeycode: the result of get() is stored, then its address is passed to get()).
const char kGetUsage[] = "_ZN9hmiplugin3getI17IHmiVariantServerEENS_25CPluginInstanceCacheUsageIT_EEv";
const char kUsageGet[] = "_ZNK9hmiplugin25CPluginInstanceCacheUsageI17IHmiVariantServerE3getEv";
const unsigned kIsVariantSetSlot = 3;  // CHmiVariantServer vtable (libhmicoding.so): isVariantSet(unsigned)
const unsigned kGetRoleSlot = 21;      // getRole()

typedef uintptr_t (*GetUsageFn)();
typedef void* (*UsageGetFn)(const uintptr_t* usage);
typedef bool (*IsVariantSetFn)(void* server, unsigned id);
typedef int (*GetRoleFn)(void* server);

// iDrive's variant server and the two calls asked of it.
class VariantServer {
public:
    VariantServer() : self_(nullptr), isSet_(nullptr), role_(nullptr) {}
    // False when HmiMain3D does not give it (logged when logMissing).
    bool find(bool logMissing = true) {
        GetUsageFn getUsage = reinterpret_cast<GetUsageFn>(dlsym(RTLD_DEFAULT, kGetUsage));
        UsageGetFn usageGet = reinterpret_cast<UsageGetFn>(dlsym(RTLD_DEFAULT, kUsageGet));
        if (getUsage && usageGet) {
            const uintptr_t usage = getUsage();
            self_ = usageGet(&usage);
        }
        void** vtable = self_ ? *reinterpret_cast<void***>(self_) : nullptr;
        if (!vtable || !vtable[kIsVariantSetSlot] || !vtable[kGetRoleSlot]) {
            if (logMissing) {
                alog("iDrive coding: variant server not found (%s %s): the service keeps G-style keys and a BMW",
                     getUsage ? "get" : "no get", usageGet ? "usage" : "no usage");
            }
            return false;
        }
        isSet_ = reinterpret_cast<IsVariantSetFn>(vtable[kIsVariantSetSlot]);
        role_ = reinterpret_cast<GetRoleFn>(vtable[kGetRoleSlot]);
        return true;
    }
    bool isSet(unsigned id) const { return isSet_(self_, id); }
    int role() const { return role_(self_); }

private:
    void* self_;
    IsVariantSetFn isSet_;
    GetRoleFn role_;
};

// iDrive's driving mode device. hmiplugin::get<IDevCarFES>() is not exported, but its steps are, the same as in
// get<IHmiVariantServer>() @0x2526d38 and CCarFesModeService::init @0x2c49b38 (G20, 202111011321): the instance cache
// of that name (libhmiplugin.so), this thread's entry (cache vtable slot 2), filled on demand by request(), and the
// instance at entry + 8.
const char kCacheByName[] = "_ZN9hmiplugin28getInstanceCacheByIdentifierEPKc";
const char kRequestFes[] = "_ZN9hmiplugin23CPluginInstanceCacheTLS7requestI10IDevCarFESEEvv";
const unsigned kCacheEntrySlot = 2;
const size_t kEntryInstanceOffset = 8;
// IDevCarFES::isPopupAvailable(), slot 23: this thunk in the vtables of CDevCarFES, CDevCarFES35 and CDevCarFESRueko.
// It reads the setting (CDevCarFES + 0x178), then isDeviceCoded(), which asks the variant server as above.
const char kIsPopupAvailable[] = "_ZThn28_NK10CDevCarFES16isPopupAvailableEv";
const unsigned kIsPopupAvailableSlot = 23;
const unsigned kVariantForcedPopup = 0x519d;  // CDevCarFES35::onFESVehicleDynamicSwitchHMIStatus: popup anyway

typedef void* (*CacheByNameFn)(const char* name);
typedef void* (*CacheEntryFn)(void* cache);
typedef void (*RequestFn)(void* entry);
typedef bool (*IsPopupAvailableFn)(const void* device);

// The setting: 1, 0, or -1 when the device or the function is not found.
int readPopupSetting() {
    CacheByNameFn byName = reinterpret_cast<CacheByNameFn>(dlsym(RTLD_DEFAULT, kCacheByName));
    RequestFn request = reinterpret_cast<RequestFn>(dlsym(RTLD_DEFAULT, kRequestFes));
    void* const isAvailable = dlsym(RTLD_DEFAULT, kIsPopupAvailable);
    if (!byName || !request || !isAvailable) {
        return -1;
    }
    void* const cache = byName("IDevCarFES");
    void** const cacheVtable = cache ? *static_cast<void***>(cache) : nullptr;
    if (!cacheVtable || !cacheVtable[kCacheEntrySlot]) {
        return -1;
    }
    void* const entry = reinterpret_cast<CacheEntryFn>(cacheVtable[kCacheEntrySlot])(cache);
    if (!entry) {
        return -1;
    }
    void** const instance = reinterpret_cast<void**>(static_cast<char*>(entry) + kEntryInstanceOffset);
    if (!*instance) {
        request(entry);
    }
    void* const device = *instance;
    void** const vtable = device ? *static_cast<void***>(device) : nullptr;
    if (!vtable || vtable[kIsPopupAvailableSlot] != isAvailable) {
        return -1;  // another device, or another layout: not called
    }
    return reinterpret_cast<IsPopupAvailableFn>(isAvailable)(device) ? 1 : 0;
}

const char* readText(int value) {
    return value < 0 ? "unread" : (value ? "on" : "off");
}

// One line to path, through a temporary file renamed over it: the service never reads half a line. On failure the
// file is removed, so the service falls back to its default rather than an old value.
bool writeLine(const char* path, const char* line) {
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    bool written = f && fprintf(f, "%s\n", line) > 0;
    if (f && fclose(f) != 0) {
        written = false;
    }
    if (!written || rename(tmp.c_str(), path) != 0) {
        unlink(tmp.c_str());
        unlink(path);
        return false;
    }
    return true;
}

bool writeControllerCoding(const VariantServer& server) {
    aa::ControllerCoding coding = aa::defaultControllerCoding();
    coding.known = true;
    coding.gStyle = server.isSet(aa::kVariantGStyle);
    coding.v5141 = server.isSet(aa::kVariant5141);
    coding.v4e59 = server.isSet(aa::kVariant4E59);
    coding.v4f1e = server.isSet(aa::kVariant4F1E);
    coding.v5486 = server.isSet(aa::kVariant5486);
    coding.role = server.role();
    char line[96];
    aa::formatControllerCoding(coding, line, sizeof(line));
    const bool written = writeLine(contract::kControllerFile, line);
    alog("controller coding: %s (%s controller)%s", line, coding.gStyle ? "G-style" : "F-style",
         written ? "" : ", not written");
    return written;
}

bool writeBrand(const VariantServer& server) {
    const aa::CarBrand brand = server.isSet(aa::kVariantBrandMini) ? aa::CarBrand::kMini : aa::CarBrand::kBmw;
    const bool written = writeLine(contract::kBrandFile, aa::brandName(brand));
    alog("brand: %s%s", aa::brandName(brand), written ? "" : ", not written");
    return written;
}

// Both variants are asked: a coding with neither or both says nothing sure, and the file is not written.
bool writeDriverSide(const VariantServer& server) {
    const bool left = server.isSet(aa::kVariantLeftHandDrive);
    const bool right = server.isSet(aa::kVariantRightHandDrive);
    if (left == right) {
        unlink(contract::kDriverSideFile);
        alog("driver side: coding unclear (left %d, right %d): the vehicle file's side, or left", left ? 1 : 0,
             right ? 1 : 0);
        return false;
    }
    const bool written = writeLine(contract::kDriverSideFile, aa::driverSideName(right));
    alog("driver side: %s%s", aa::driverSideName(right), written ? "" : ", not written");
    return written;
}

// Every series is asked: exactly one set, and not "unknown", is written; anything else says nothing sure.
bool writeSeries(const VariantServer& server) {
    const char* series = nullptr;
    unsigned set = 0;
    for (size_t i = 0; i < aa::kSeriesVariantCount; ++i) {
        if (server.isSet(aa::kSeriesVariants[i].id)) {
            series = aa::kSeriesVariants[i].series;
            ++set;
        }
    }
    const bool unknown = server.isSet(aa::kVariantSeriesUnknown);
    if (set != 1 || unknown) {
        unlink(contract::kSeriesFile);
        alog("series: coding unclear (%u series set%s): the vehicle file's model, or iDrive", set,
             unknown ? ", unknown set" : "");
        return false;
    }
    const bool written = writeLine(contract::kSeriesFile, series);
    alog("series: %s%s", series, written ? "" : ", not written");
    return written;
}

}  // namespace

bool writeIdriveCoding() {
    VariantServer server;
    if (!server.find()) {
        unlink(contract::kControllerFile);
        unlink(contract::kBrandFile);
        unlink(contract::kDriverSideFile);
        unlink(contract::kSeriesFile);
        return false;
    }
    const bool keys = writeControllerCoding(server);
    const bool brand = writeBrand(server);
    const bool side = writeDriverSide(server);
    const bool series = writeSeries(server);
    return keys && brand && side && series;
}

DriveModePopup readDriveModePopup() {
    DriveModePopup popup;
    popup.setting = readPopupSetting();
    VariantServer server;
    popup.forced = server.find(false) ? (server.isSet(kVariantForcedPopup) ? 1 : 0) : -1;
    alog("iDrive driving mode popup: setting %s, forced by coding %s", readText(popup.setting), readText(popup.forced));
    return popup;
}

}  // namespace app
}  // namespace evo
