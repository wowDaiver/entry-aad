// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The Wi-Fi link's figures: station messages as the kernel builds them, the log line, TCP on loopback, and the copy
// of the phone's connection the log keeps and gives back. No Wi-Fi interface is read.
#include <arpa/inet.h>
#include <linux/nl80211.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "wifi_link.hpp"

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                            \
        }                                                       \
    } while (0)

namespace {

// Netlink attributes: length (header included), type, value, padded to 4 bytes.
struct Attrs {
    std::vector<uint8_t> bytes;
    void put(uint16_t type, const void* data, size_t len) {
        const uint16_t total = static_cast<uint16_t>(4 + len);
        const uint8_t* d = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&total),
                     reinterpret_cast<const uint8_t*>(&total) + 2);
        bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&type), reinterpret_cast<const uint8_t*>(&type) + 2);
        bytes.insert(bytes.end(), d, d + len);
        bytes.resize((bytes.size() + 3) & ~static_cast<size_t>(3), 0);
    }
    void u8(uint16_t type, uint8_t v) { put(type, &v, 1); }
    void u16(uint16_t type, uint16_t v) { put(type, &v, 2); }
    void u32(uint16_t type, uint32_t v) { put(type, &v, 4); }
    void nested(uint16_t type, const Attrs& inside) {
        put(static_cast<uint16_t>(type | 0x8000), inside.bytes.empty() ? nullptr : &inside.bytes[0],
            inside.bytes.size());
    }
};

const uint8_t kPhone[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};

Attrs rate(uint16_t tenths) {
    Attrs r;
    r.u16(NL80211_RATE_INFO_BITRATE, tenths);
    r.u8(NL80211_RATE_INFO_MCS, 7);
    return r;
}

Attrs stationMessage(uint32_t frames, uint32_t retries, uint32_t failed, uint32_t connected) {
    Attrs info;
    info.u32(NL80211_STA_INFO_INACTIVE_TIME, 12);
    info.u32(NL80211_STA_INFO_RX_BYTES, 123456);
    info.u8(NL80211_STA_INFO_SIGNAL, static_cast<uint8_t>(-52));
    info.u8(NL80211_STA_INFO_SIGNAL_AVG, static_cast<uint8_t>(-51));
    info.nested(NL80211_STA_INFO_TX_BITRATE, rate(1500));
    info.nested(NL80211_STA_INFO_RX_BITRATE, rate(1350));
    info.u32(NL80211_STA_INFO_TX_PACKETS, frames);
    info.u32(NL80211_STA_INFO_TX_RETRIES, retries);
    info.u32(NL80211_STA_INFO_TX_FAILED, failed);
    info.u32(NL80211_STA_INFO_CONNECTED_TIME, connected);
    Attrs message;
    message.u32(NL80211_ATTR_IFINDEX, 4);
    message.put(NL80211_ATTR_MAC, kPhone, sizeof(kPhone));
    message.nested(NL80211_ATTR_STA_INFO, info);
    return message;
}

evo::WifiStation station(uint32_t frames, uint32_t retries, uint32_t failed, uint32_t connected) {
    const Attrs m = stationMessage(frames, retries, failed, connected);
    evo::WifiStation s;
    CHECK(evo::parseWifiStation(&m.bytes[0], m.bytes.size(), s));
    return s;
}

void stationMessages() {
    const evo::WifiStation s = station(5000, 300, 2, 125);
    CHECK(!memcmp(s.mac, kPhone, sizeof(kPhone)));
    CHECK(s.signalDbm == -52 && s.signalAvgDbm == -51 && s.txRate == 1500 && s.rxRate == 1350);
    CHECK(s.txFrames == 5000 && s.txRetries == 300 && s.txFailed == 2 && s.connectedS == 125);

    // A driver that gives little: what is missing stays "no figure".
    Attrs info;
    info.u8(NL80211_STA_INFO_SIGNAL, static_cast<uint8_t>(-70));
    info.u16(NL80211_STA_INFO_TX_PACKETS, 9);  // not the size nl80211 gives it: ignored
    Attrs bare;
    bare.put(NL80211_ATTR_MAC, kPhone, sizeof(kPhone));
    bare.nested(NL80211_ATTR_STA_INFO, info);
    evo::WifiStation little;
    CHECK(evo::parseWifiStation(&bare.bytes[0], bare.bytes.size(), little));
    CHECK(little.signalDbm == -70 && little.signalAvgDbm == evo::kNoFigure && little.txRate == evo::kNoFigure &&
          little.txFrames == evo::kNoFigure && little.connectedS == evo::kNoFigure);

    // No station address: not a station.
    Attrs anonymous;
    anonymous.nested(NL80211_ATTR_STA_INFO, info);
    CHECK(!evo::parseWifiStation(&anonymous.bytes[0], anonymous.bytes.size(), little));

    // Cut anywhere, or with a length that lies: never read past the end.
    const Attrs whole = stationMessage(1, 2, 3, 4);
    for (size_t cut = 0; cut < whole.bytes.size(); ++cut) {
        std::vector<uint8_t> part(whole.bytes.begin(), whole.bytes.begin() + cut);
        part.reserve(cut + 1);
        evo::parseWifiStation(part.empty() ? nullptr : &part[0], part.size(), little);
    }
    std::vector<uint8_t> lying = whole.bytes;
    lying[0] = 0xff;
    lying[1] = 0xff;
    CHECK(!evo::parseWifiStation(&lying[0], lying.size(), little));
    lying = whole.bytes;
    lying[0] = 2;  // shorter than its own header
    lying[1] = 0;
    CHECK(!evo::parseWifiStation(&lying[0], lying.size(), little));

    Attrs interface;
    interface.u32(NL80211_ATTR_IFINDEX, 4);
    interface.u32(NL80211_ATTR_WIPHY_FREQ, 5180);
    unsigned mhz = 0;
    CHECK(evo::parseWifiFrequency(&interface.bytes[0], interface.bytes.size(), mhz) && mhz == 5180);
    Attrs silent;
    silent.u32(NL80211_ATTR_IFINDEX, 4);
    CHECK(!evo::parseWifiFrequency(&silent.bytes[0], silent.bytes.size(), mhz));
}

evo::WifiSample sample(uint32_t atMs, uint32_t frames, uint32_t retries, uint32_t failed, uint32_t connected,
                       uint32_t roundTripUs, uint32_t resent) {
    evo::WifiSample s;
    s.atMs = atMs;
    s.radio = true;
    s.mhz = 5180;
    s.stations = 1;
    s.hasStation = true;
    s.station = station(frames, retries, failed, connected);
    s.hasTcp = true;
    s.tcp.roundTripUs = roundTripUs;
    s.tcp.resent = resent;
    return s;
}

void logLines() {
    const evo::WifiSample first = sample(1000, 5000, 300, 2, 125, 4250, 3);
    CHECK(evo::describeWifiLink(first, nullptr) ==
          "5180 MHz; signal -52 dBm (average -51); to the phone 150.0 Mbit/s; from the phone 135.0 Mbit/s; "
          "since it joined: 5000 frames sent, 300 retried, 2 lost; on the Wi-Fi for 125 s; "
          "TCP round trip 4.2 ms, 3 segment(s) sent again since it opened");
    // Ten seconds later: what happened in between.
    const evo::WifiSample second = sample(11040, 5812, 335, 2, 135, 38900, 9);
    CHECK(evo::describeWifiLink(second, &first) ==
          "5180 MHz; signal -52 dBm (average -51); to the phone 150.0 Mbit/s; from the phone 135.0 Mbit/s; "
          "in 10 s: 812 frames sent, 35 retried, 0 lost; on the Wi-Fi for 135 s; "
          "TCP round trip 38.9 ms, 6 segment(s) sent again");
    // The phone left the Wi-Fi and came back: its counters started again, nothing is subtracted.
    const evo::WifiSample back = sample(21000, 40, 1, 0, 3, 4000, 9);
    const std::string again = evo::describeWifiLink(back, &second);
    CHECK(again.find("the phone joined the Wi-Fi again; since it joined: 40 frames sent, 1 retried, 0 lost") !=
          std::string::npos);
    CHECK(again.find("on the Wi-Fi for 3 s") != std::string::npos);
    // Another phone's entry is never subtracted from either.
    evo::WifiSample other = sample(31000, 9000, 900, 9, 600, 4000, 9);
    other.station.mac[5] ^= 1;
    CHECK(evo::describeWifiLink(other, &back).find("joined the Wi-Fi again") != std::string::npos);

    // The phone is not among the stations (two phones on the access point, neither recognised).
    evo::WifiSample lost = first;
    lost.hasStation = false;
    lost.stations = 2;
    CHECK(evo::describeWifiLink(lost, &first) ==
          "5180 MHz; phone not found among the 2 station(s) of the access point; "
          "TCP round trip 4.2 ms, 0 segment(s) sent again");
    // The kernel gave no Wi-Fi figure: TCP alone. Neither: no line.
    evo::WifiSample tcpOnly;
    tcpOnly.hasTcp = true;
    tcpOnly.tcp.roundTripUs = 950;
    tcpOnly.tcp.resent = 0;
    CHECK(evo::describeWifiLink(tcpOnly, nullptr) == "TCP round trip 0.9 ms, 0 segment(s) sent again since it opened");
    CHECK(evo::describeWifiLink(evo::WifiSample(), nullptr).empty());
    // A driver without counters: no "frames" part at all.
    evo::WifiSample bare = first;
    bare.station.txFrames = bare.station.txRetries = bare.station.txFailed = bare.station.connectedS = evo::kNoFigure;
    bare.station.signalAvgDbm = bare.station.rxRate = evo::kNoFigure;
    bare.hasTcp = false;
    CHECK(evo::describeWifiLink(bare, nullptr) == "5180 MHz; signal -52 dBm; to the phone 150.0 Mbit/s");
}

// A TCP connection on loopback: the car's end in car, the phone's in phone.
void connectPair(int& car, int& phone) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    sockaddr_in at = sockaddr_in();
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(at);
    CHECK(bind(listener, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0 && listen(listener, 1) == 0);
    CHECK(getsockname(listener, reinterpret_cast<sockaddr*>(&at), &len) == 0);
    phone = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(phone >= 0 && connect(phone, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0);
    car = accept(listener, nullptr, nullptr);
    CHECK(car >= 0);
    close(listener);
}

// The phone's end reads "closed": every copy of the car's end is gone.
bool closedForPhone(int phone) {
    pollfd p;
    p.fd = phone;
    p.events = POLLIN;
    p.revents = 0;
    char byte;
    return poll(&p, 1, 0) == 1 && recv(phone, &byte, 1, MSG_DONTWAIT) == 0;
}

void tcpFigures() {
    int car = -1, phone = -1;
    connectPair(car, phone);
    CHECK(send(car, "hello", 5, 0) == 5);
    char buffer[8];
    CHECK(recv(phone, buffer, sizeof(buffer), 0) == 5);
    evo::TcpLink link;
    CHECK(evo::readTcpLink(car, link) && link.resent == 0 && link.roundTripUs < 1000000);
    close(car);
    close(phone);
    // A USB session's local socket has no TCP figures.
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(!evo::readTcpLink(pair[0], link) && !evo::readTcpLink(-1, link));
    close(pair[0]);
    close(pair[1]);
}

void connectionCopy() {
    {  // Not enabled (production): no copy is ever kept.
        int car = -1, phone = -1;
        connectPair(car, phone);
        evo::WifiLinkLog log;
        log.connected(car);
        close(car);
        CHECK(closedForPhone(phone));
        close(phone);
    }
    {  // A session: logged while it runs, the copy given back at the first turn after its end.
        int car = -1, phone = -1;
        connectPair(car, phone);
        evo::WifiLinkLog log;
        log.enable("entry-aad-no-such-interface");  // nl80211 has nothing to say here: the TCP figures remain
        log.connected(car);
        close(car);  // as the session does at its end; the log's copy keeps the socket open
        log.turn(1000, true);
        log.turn(1100, true);
        CHECK(!closedForPhone(phone));
        log.turn(1200, false);
        CHECK(closedForPhone(phone));
        log.turn(1300, false);  // nothing left to do
        close(phone);
    }
    {  // A connection whose session never starts: kept for the start's few seconds, then given back.
        int car = -1, phone = -1;
        connectPair(car, phone);
        evo::WifiLinkLog log;
        log.enable("entry-aad-no-such-interface");
        log.connected(car);
        close(car);
        log.turn(0xfffffff0u, false);  // the 32-bit clock wraps during the wait
        log.turn(0xfffffff0u + 4000, false);
        CHECK(!closedForPhone(phone));
        log.turn(0xfffffff0u + 5000, false);
        CHECK(closedForPhone(phone));
        close(phone);
    }
    {  // A new connection replaces the one before; the log's end gives the last copy back.
        int car = -1, phone = -1, car2 = -1, phone2 = -1;
        connectPair(car, phone);
        connectPair(car2, phone2);
        {
            evo::WifiLinkLog log;
            log.enable("entry-aad-no-such-interface");
            log.connected(car);
            close(car);
            log.connected(car2);
            close(car2);
            CHECK(closedForPhone(phone) && !closedForPhone(phone2));
        }
        CHECK(closedForPhone(phone2));
        close(phone);
        close(phone2);
    }
}

}  // namespace

int main() {
    stationMessages();
    logLines();
    tcpFigures();
    connectionCopy();
    puts("PASS Wi-Fi link: station messages, log lines, TCP figures, the connection's copy");
    return 0;
}
