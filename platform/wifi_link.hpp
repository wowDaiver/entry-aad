// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// How good the Wi-Fi link to the phone is, written to the service's log while Android Auto runs wireless.

// Everything here only reads. Two sources, both the kernel's own figures:
//   - nl80211 (generic netlink), the interface `iw` uses: the phone's station entry on the car's access point (signal,
//     bitrate from the phone, frames sent, retried and lost) and the access point's frequency;
//     https://www.kernel.org/doc/html/latest/userspace-api/netlink/intro.html, include/uapi/linux/nl80211.h
//     The rate towards the phone is left out: the WL18xx firmware chooses it, and the driver (wlcore) reported
//     1 Mbit/s on every car, the reference G20 included, whatever was sent.
//   - TCP_INFO on the Android Auto connection: round trip time and segments sent again.
// Next to them, given by the service: whether a phone holds BMW Apps' Bluetooth link, which shares the radio chip.
// Nothing identifies the phone in the log: no address is written.
#ifndef ENTRY_AAD_PLATFORM_WIFI_LINK_HPP
#define ENTRY_AAD_PLATFORM_WIFI_LINK_HPP

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace evo {

const int64_t kNoFigure = -0x7fffffffffffffffLL - 1;  // a figure the driver did not give

// One phone on the access point, as nl80211 reports it.
struct WifiStation {
    uint8_t mac[6];
    int64_t signalDbm, signalAvgDbm;        // signalDbm: the last frame received, when the chip measured it
    int64_t rxRate;                         // in 100 kbit/s
    int64_t txFrames, txRetries, txFailed;  // since the phone joined
    int64_t connectedS;                     // how long ago that was
    WifiStation();
};

// The attributes of one nl80211 "new station" message: what follows its generic netlink header. False without a
// station address.
bool parseWifiStation(const uint8_t* attrs, size_t len, WifiStation& out);
// The frequency, in MHz, among the attributes of an nl80211 "new interface" message.
bool parseWifiFrequency(const uint8_t* attrs, size_t len, unsigned& mhz);

// The Android Auto connection as TCP sees it.
struct TcpLink {
    uint32_t roundTripUs;
    uint32_t resent;  // segments sent again since the connection opened
};
// False when fd is not a TCP connection (a USB session uses a local socket).
bool readTcpLink(int fd, TcpLink& out);

// BMW Apps' Bluetooth slot (SPP 0: My BMW, AAIdrive, the phone's "Apps" option in the car's device settings). The
// WL18xx chip carries both Bluetooth and Wi-Fi; wireless Android Auto was slow to start with Apps on (#71).
struct AppsSlot {
    bool read;          // false: not read (Android Auto uses SPP 0 itself, or the Alps tables were unreadable)
    bool connected;     // a phone holds a link on it
    uint32_t state;     // the slot's Alps state (1: started)
    uint8_t transport;  // its serial link's state (0: none)
    AppsSlot() : read(false), connected(false), state(0), transport(0) {}
};

struct WifiSample {
    uint32_t atMs;
    bool radio;         // nl80211 answered: the three fields below mean something
    unsigned mhz;       // 0: unknown
    unsigned stations;  // phones on the access point
    bool hasStation;    // the Android Auto phone was found among them
    WifiStation station;
    bool hasTcp;
    TcpLink tcp;
    AppsSlot apps;
    WifiSample();
};

// The log line of one sample. before: the previous sample of the same connection, for what happened in between;
// null for the first one.
std::string describeWifiLink(const WifiSample& now, const WifiSample* before);

// Logs one line when the phone's connection opens, then one every 10 seconds until the session ends.
class WifiLinkLog {
public:
    WifiLinkLog();
    ~WifiLinkLog();
    // The access point's interface. Without this call nothing is ever read or logged.
    void enable(const char* interface);
    // Reads BMW Apps' slot for each line, from the thread that calls turn(); false when it could not be read.
    // Without it, the lines say nothing about Bluetooth.
    typedef bool (*AppsProbe)(void* user, AppsSlot& out);
    void setAppsProbe(AppsProbe probe, void* user);
    // The phone's TCP connection, from the thread that accepted it. fd stays the caller's: a copy is kept.
    void connected(int fd);
    // Main loop, every turn. The copy is closed as soon as the session is over.
    void turn(uint32_t nowMs, bool sessionActive);

private:
    WifiLinkLog(const WifiLinkLog&);
    WifiLinkLog& operator=(const WifiLinkLog&);
    void sample(uint32_t nowMs);
    void forget();

    pthread_mutex_t lock_;
    std::string interface_;
    AppsProbe appsProbe_;
    void* appsUser_;
    int fd_;  // copy of the phone's connection, -1 when none
    bool first_, seenActive_, hasBefore_, radioGone_;
    uint32_t nextMs_, sinceMs_;
    unsigned radioFailures_;
    WifiSample before_;
};

}  // namespace evo

#endif
