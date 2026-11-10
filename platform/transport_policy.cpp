// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Read only the selected physical port; unrelated USB devices are never candidates.
#include "transport_policy.hpp"

#include <sys/stat.h>

#include <cerrno>

namespace evo {
PhonePresence phonePresence(const aa::UsbSelection& selection, const char* sysfs) {
    struct stat root;
    if (stat(sysfs, &root) != 0 || !S_ISDIR(root.st_mode)) {
        return kPresenceUnknown;
    }
    if (!selection.valid()) {
        return kPresenceUnknown;
    }
    aa::UsbIdentity id;
    if (aa::readUsbIdentity(selection, id, sysfs)) {
        if (id.deviceClass == 9) {
            return kPhoneAbsent;
        }
        if (id.vendor == selection.vendor && id.product == selection.product) {
            return kPhoneNormal;
        }
        if (id.accessory()) {
            return kPhoneAccessory;
        }
        return kPhoneAbsent;  // another device: leave it entirely to BMW
    }
    struct stat st;
    if (stat((std::string(sysfs) + "/" + selection.port).c_str(), &st) != 0 && errno == ENOENT) {
        return kPhoneAbsent;
    }
    return kPresenceUnknown;
}
}  // namespace evo
