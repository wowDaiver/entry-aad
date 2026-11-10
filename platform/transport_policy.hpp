// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Read USB presence once at service startup. Cable changes do not switch an active service.
#ifndef ENTRY_AAD_TRANSPORT_POLICY_HPP
#define ENTRY_AAD_TRANSPORT_POLICY_HPP

#include <stdint.h>

#include "usb_transport.hpp"

namespace evo {
enum ProjectionTransport { kWirelessProjection, kUsbProjection };
enum PhonePresence { kPhoneAbsent, kPhoneNormal, kPhoneAccessory, kPresenceUnknown };

PhonePresence phonePresence(const aa::UsbSelection& selection, const char* sysfs = "/sys/bus/usb/devices");

inline ProjectionTransport startupTransport(PhonePresence presence) {
    return presence == kPhoneNormal || presence == kPhoneAccessory ? kUsbProjection : kWirelessProjection;
}
}  // namespace evo
#endif
