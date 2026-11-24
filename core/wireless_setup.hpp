// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Wireless Android Auto setup over Bluetooth RFCOMM: hands the car's Wi-Fi details to the phone.
#ifndef ENTRY_AAD_CORE_WIRELESS_SETUP_HPP
#define ENTRY_AAD_CORE_WIRELESS_SETUP_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace aa {

// The car's access point as the phone is told it (WifiInfoResponse).
struct ApInfo {
    std::string ssid;
    std::string key;
    std::string bssid;  // "AA:BB:CC:DD:EE:FF", upper case
    std::string ip;     // the head unit's address on it, where the phone connects
    int security;       // 8 = WPA2_PERSONAL
    int apType;         // 1 = DYNAMIC
    ApInfo() : security(8), apType(1) {}
};

// The Bluetooth half of wireless Android Auto, on one open RFCOMM link. The head unit sends WifiStartRequest (its
// address and TCP port), answers the phone's WifiInfoRequest with the access point, and waits until the phone has
// joined. A message is [u16 length][u16 id][protobuf body], big-endian. The car's RFCOMM device is message-oriented:
// a read shorter than the pending message loses the rest, so the link is read in large chunks and cut here.
class WirelessSetup {
public:
    // peekMs: listen this long before writing anything (3000 at most), so that a link of the BMW app, which speaks
    // first, gets nothing written into it. 0 writes at once.
    WirelessSetup(int fd, const ApInfo& ap, uint16_t port, int peekMs = 0);

    // True once the phone reports it joined the Wi-Fi, or closes Bluetooth after accepting the start (it is moving to
    // Wi-Fi). The phone's first answer is awaited timeoutMs (WifiStartRequest sent again, twice, when only the start
    // was accepted); after the access point went out, 45 s.
    bool handshake(int timeoutMs);
    // After the handshake: answers pings and new info requests, waiting up to waitMs for data. False once the phone
    // closed the link.
    bool serve(int waitMs);
    // A link that came back while Android Auto already runs: listens only, up to waitMs. A ping is answered;
    // anything else is written to the log and left unanswered, and nothing is sent first (a new WifiStartRequest
    // could restart the session that runs). False once the phone closed the link.
    bool listen(int waitMs);
    unsigned heard() const { return heard_; }  // messages received by listen()
    // The phone sent at least one well-formed message on this link (a working phone: WifiStartResponse at once).
    bool spokeWireless() const { return spoke_; }

private:
    enum Fill { kGone = -1, kQuiet = 0, kData = 1 };
    enum Step { kWriteFailed = -1, kGoOn = 0, kJoined = 1 };
    struct Progress {
        bool infoSent;     // the phone asked for the access point and got it
        int startAnswers;  // WifiStartResponses before that
        bool startAccepted;
    };

    Fill fill(int waitMs);
    bool next(uint16_t& id, std::vector<uint8_t>& body);
    Step handle(uint16_t id, const std::vector<uint8_t>& body, Progress& progress);
    bool write(const std::vector<uint8_t>& message);

    int fd_;
    ApInfo ap_;
    uint16_t port_;
    int peekMs_;
    std::vector<uint8_t> pending_;  // read, not yet cut into messages
    bool foreign_;                  // the link turned out to be the BMW app's
    bool spoke_;
    unsigned heard_;
};

// True when the bytes are the BMW app's (MyBMW, BMW Connected), not Android Auto's: its 4-byte startup magic
// 12 34 56 78, an Etch DEADBEEF or 1337 watchdog payload, or a self-consistent BCL frame.
bool looksLikeBmwBcl(const uint8_t* p, size_t n);

}  // namespace aa

#endif  // ENTRY_AAD_CORE_WIRELESS_SETUP_HPP
