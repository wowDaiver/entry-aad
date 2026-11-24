// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's access point offers no U-APSD to the phone that joins it (#138).
//
// wland writes uapsd_advertisement_enabled=1 into hostapd's configuration. On an X1 F48 the WL18xx then held a phone's
// frames back for seconds while it saved power (firmware counter ps_poll_upsd_timeouts: 1700 in 12 minutes), and the
// picture and the sound stalled; with U-APSD off, the stalls were gone.
//
// hostapd on the car is v2.1-devel. SET changes the setting in memory only (hostapd_set_iface): association responses
// follow it at once, but the beacon and the probe responses, which the chip sends from a template, go on offering
// U-APSD until hostapd rebuilds them. A phone asks for U-APSD from them in its association request, and hostapd keeps
// what it asked (check_wmm). RELOAD rebuilds them from the setting in memory, not from the file
// (hostapd_reload_iface), but disconnects every station first (hostapd_clear_old). So it is sent only when no station
// is associated: an iPhone on CarPlay, or a phone already on the car's Wi-Fi, is left alone and keeps U-APSD until it
// joins again. wland can start hostapd again from its own file: the setting is made before each Wi-Fi exchange with a
// phone, not once. RELOAD itself is sent once per hostapd start: a phone that comes back reopens its Bluetooth link
// every 400 ms or so while it joins the Wi-Fi again, and a RELOAD each time slowed its return from 2 s to 3-7 s on an
// F45 (one of them may well have caught it joining).
//
// Pure: the hostapd_cli calls are made by the caller (platform/evo_ap.cpp). The station list is only checked for being
// empty, never logged (MAC addresses). tests/test_parsers.cpp.
#ifndef ENTRY_AAD_PLATFORM_AP_UAPSD_HPP
#define ENTRY_AAD_PLATFORM_AP_UAPSD_HPP

#include <string>
#include <vector>

namespace evo {

enum class UapsdOffer {
    kWithdrawn,      // off, and the beacon rebuilt: the next phone joins without U-APSD
    kAlreadyDone,    // off, the beacon rebuilt earlier for this hostapd start: no RELOAD again
    kBeaconLeft,     // off for new associations; a station is associated (or the list unreadable): no RELOAD
    kReloadRefused,  // off in hostapd, but RELOAD refused or unanswered: the beacon may still offer it
    kSetRefused,     // hostapd refused the setting or did not answer: left on
};

inline const char* describeUapsdOffer(UapsdOffer offer) {
    switch (offer) {
        case UapsdOffer::kWithdrawn:
            return "U-APSD off, beacon rebuilt: the phone joins without it";
        case UapsdOffer::kAlreadyDone:
            return "U-APSD off, beacon already rebuilt for this hostapd start";
        case UapsdOffer::kBeaconLeft:
            return "U-APSD off for new associations; a station is associated, beacon left, nobody disconnected";
        case UapsdOffer::kReloadRefused:
            return "U-APSD off in hostapd, RELOAD refused: the beacon may still offer it";
        case UapsdOffer::kSetRefused:
            break;
    }
    return "hostapd refused the setting: U-APSD left on";
}

namespace uapsd {
inline bool blank(const std::string& reply) {
    return reply.find_first_not_of(" \t\r\n") == std::string::npos;
}
// hostapd_cli prints the reply as it is: "OK" or "FAIL", an end of line after it.
inline bool ok(const std::string& reply) {
    const size_t end = reply.find_last_not_of(" \t\r\n");
    return end != std::string::npos && reply.compare(0, end + 1, "OK") == 0;
}
}  // namespace uapsd

// Which hostapd start this is: its control socket, created anew each time hostapd starts (unlinked, then bound).
struct HostapdStart {
    unsigned long long device, inode;
    bool known;
    HostapdStart() : device(0), inode(0), known(false) {}
    bool same(const HostapdStart& other) const {
        return known && other.known && device == other.device && inode == other.inode;
    }
};

// call(command, reply): runs one hostapd_cli command on wlan0 ({"set", ...}) and gives its reply; false when it could
// not run. At most three calls: the setting, the station list, RELOAD. beaconDone: the beacon was rebuilt for this
// hostapd start already (the caller knows it from HostapdStart): the setting only.
template <typename Call>
UapsdOffer withdrawUapsd(Call call, bool beaconDone = false) {
    std::string reply;
    if (!call(std::vector<std::string>{"set", "uapsd_advertisement_enabled", "0"}, reply) || !uapsd::ok(reply)) {
        return UapsdOffer::kSetRefused;
    }
    if (beaconDone) {
        return UapsdOffer::kAlreadyDone;
    }
    if (!call(std::vector<std::string>{"all_sta"}, reply) || !uapsd::blank(reply)) {
        return UapsdOffer::kBeaconLeft;
    }
    if (!call(std::vector<std::string>{"raw", "RELOAD"}, reply) || !uapsd::ok(reply)) {
        return UapsdOffer::kReloadRefused;
    }
    return UapsdOffer::kWithdrawn;
}

}  // namespace evo

#endif  // ENTRY_AAD_PLATFORM_AP_UAPSD_HPP
