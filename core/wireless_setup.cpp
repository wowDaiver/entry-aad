// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Wireless Android Auto setup over Bluetooth RFCOMM: messages, the handshake, and the BMW app's link told apart.
#include "wireless_setup.hpp"

#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "pb.hpp"

namespace aa {
namespace {

// Message ids, both ways.
const uint16_t kStartRequest = 1;     // head unit -> phone {1 ip, 2 port}; also sent by some phones
const uint16_t kInfoRequest = 2;      // phone -> head unit, empty
const uint16_t kInfoResponse = 3;     // head unit -> phone {1 ssid, 2 key, 3 bssid, 4 security, 5 access point type}
const uint16_t kVersionResponse = 5;  // phone -> head unit: the phone speaks the newer protocol
const uint16_t kConnectStatus = 6;    // phone -> head unit {1 status}
const uint16_t kStartResponse = 7;    // both ways {1 ip, 2 port, 3 or 4 status}
const uint16_t kPingRequest = 8;      // phone -> head unit, echoed back as kPingResponse
const uint16_t kPingResponse = 9;
const uint16_t kLastId = 32;  // anything above, or 0, is not a message boundary

const size_t kHeaderSize = 4;
const size_t kMaxBody = 4096;
const size_t kMaxPending = 16384;
const size_t kReadChunk = 2048;
const int kMaxPeekMs = 3000;
const int kWaitMs = 1000;
const uint64_t kJoinWaitMs = 45000;  // after the access point went out
const int kResends = 2;

uint64_t monotonicMs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec / 1000000);
}

unsigned bigEndian16(const uint8_t* p) {
    return (static_cast<unsigned>(p[0]) << 8) | p[1];
}

std::vector<uint8_t> wireMessage(uint16_t id, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> message;
    message.push_back(static_cast<uint8_t>(body.size() >> 8));
    message.push_back(static_cast<uint8_t>(body.size() & 0xFF));
    message.push_back(static_cast<uint8_t>(id >> 8));
    message.push_back(static_cast<uint8_t>(id & 0xFF));
    message.insert(message.end(), body.begin(), body.end());
    return message;
}

std::vector<uint8_t> startRequest(const ApInfo& ap, uint16_t port) {
    std::vector<uint8_t> body;
    pb::appendString(body, 1, ap.ip);
    pb::appendUint(body, 2, port);
    return wireMessage(kStartRequest, body);
}

std::vector<uint8_t> infoResponse(const ApInfo& ap) {
    std::vector<uint8_t> body;
    pb::appendString(body, 1, ap.ssid);
    pb::appendString(body, 2, ap.key);
    pb::appendString(body, 3, ap.bssid);
    pb::appendUint(body, 4, static_cast<uint64_t>(ap.security));
    pb::appendUint(body, 5, static_cast<uint64_t>(ap.apType));
    return wireMessage(kInfoResponse, body);
}

// The answer to a start request sent by the phone itself: our address, port, and status 0 (the phone's own is `18 00`).
std::vector<uint8_t> startResponse(const ApInfo& ap, uint16_t port) {
    std::vector<uint8_t> body;
    pb::appendString(body, 1, ap.ip);
    pb::appendUint(body, 2, port);
    pb::appendUint(body, 3, 0);
    return wireMessage(kStartResponse, body);
}

bool field(const std::vector<uint8_t>& body, uint32_t number, uint64_t& value) {
    return !body.empty() && pb::findVarint(&body[0], body.size(), number, value);
}

void printBytes(const char* what, const uint8_t* p, size_t n, size_t shown) {
    printf("[Wireless] %s (%u bytes):", what, static_cast<unsigned>(n));
    for (size_t i = 0; i < n && i < shown; ++i) {
        printf(" %02x", p[i]);
    }
    printf("\n");
}

// The fields as they came, without a schema: what the phone really sent.
void printFields(const std::vector<uint8_t>& body) {
    printf("[Wireless]    fields:");
    pb::FieldReader reader(&body[0], body.size());
    pb::Field f;
    while (reader.next(f)) {
        if (f.type == pb::kVarint) {
            printf(" f%u=%llu", f.number, static_cast<unsigned long long>(f.value));
        } else if (f.type != pb::kBytes) {
            printf(" f%u=<fixed%d>", f.number, f.type == pb::kFixed32 ? 32 : 64);
        } else {
            bool text = true;
            for (size_t i = 0; i < f.size; ++i) {
                text = text && f.data[i] >= 32 && f.data[i] <= 126;
            }
            if (text) {
                printf(" f%u='%.*s'", f.number, static_cast<int>(f.size), reinterpret_cast<const char*>(f.data));
            } else {
                printf(" f%u=<%u bytes>", f.number, static_cast<unsigned>(f.size));
            }
        }
    }
    printf(reader.malformed() ? " <malformed>\n" : "\n");
}

}  // namespace

// The BMW app talks BCL over its own serial link: an 8-byte header (u16 command, u16 source port, u16 destination
// port, u16 length), carrying Apache Etch packets that start with DEADBEEF (seen: 00 02 00 11 0f a4 00 3c de ad be ef,
// DATA to port 4004), or the watchdog alone (00 02 13 89 13 89 00 04 13 37 13 37). It opens with a bare startup magic
// 12 34 56 78; links that sent only that, twice, never answered our start request, and the Wi-Fi details went to the
// BMW app until this check. A well-formed Android Auto message is never taken for it, whatever its body holds.
bool looksLikeBmwBcl(const uint8_t* p, size_t n) {
    if (!p || n < 4) {
        return false;
    }
    if (p[0] == 0x12 && p[1] == 0x34 && p[2] == 0x56 && p[3] == 0x78) {
        return true;
    }
    const unsigned first = bigEndian16(p), id = bigEndian16(p + 2);
    if (first == n - kHeaderSize && id >= 1 && id <= kLastId) {
        return false;
    }
    // The signatures sit at the start of the payload only: scanning every byte took Android Auto messages for them.
    if (n >= 12 && ((p[8] == 0xde && p[9] == 0xad && p[10] == 0xbe && p[11] == 0xef) ||
                    (p[8] == 0x13 && p[9] == 0x37 && p[10] == 0x13 && p[11] == 0x37))) {
        return true;
    }
    return n >= 8 && first >= 1 && first <= 0x20 && bigEndian16(p + 6) == n - 8;
}

WirelessSetup::WirelessSetup(int fd, const ApInfo& ap, uint16_t port, int peekMs)
    : fd_(fd),
      ap_(ap),
      port_(port),
      peekMs_(peekMs < 0 ? 0 : (peekMs > kMaxPeekMs ? kMaxPeekMs : peekMs)),
      foreign_(false),
      spoke_(false),
      heard_(0) {}

bool WirelessSetup::write(const std::vector<uint8_t>& message) {
    printf("[Wireless] RAW TX t=%llu +%u\n", static_cast<unsigned long long>(monotonicMs()),
           static_cast<unsigned>(message.size()));
    size_t done = 0;
    while (done < message.size()) {
        const ssize_t n = ::write(fd_, &message[done], message.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                usleep(10000);
                continue;
            }
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

WirelessSetup::Fill WirelessSetup::fill(int waitMs) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd_, &readable);
    timeval wait;
    wait.tv_sec = waitMs / 1000;
    wait.tv_usec = (waitMs % 1000) * 1000;
    const int ready = select(fd_ + 1, &readable, nullptr, nullptr, &wait);
    if (ready == 0) {
        return kQuiet;
    }
    if (ready < 0) {
        return errno == EINTR ? kQuiet : kGone;
    }
    uint8_t chunk[kReadChunk];
    const ssize_t n = read(fd_, chunk, sizeof(chunk));
    if (n == 0) {
        return kGone;
    }
    if (n < 0) {
        return errno == EINTR || errno == EAGAIN ? kQuiet : kGone;
    }
    printf("[Wireless] RAW RX t=%llu +%d:", static_cast<unsigned long long>(monotonicMs()), static_cast<int>(n));
    for (ssize_t i = 0; i < n && i < 48; ++i) {
        printf(" %02x", chunk[i]);
    }
    printf("\n");
    // Only a link's first data is checked: after an Android Auto message, a passphrase or a song title may hold
    // anything.
    if (!spoke_ && pending_.empty() && looksLikeBmwBcl(chunk, static_cast<size_t>(n))) {
        printf("[Wireless] this Bluetooth serial link is the BMW companion app (BCL), not Android Auto\n");
        foreign_ = true;
        return kGone;
    }
    const size_t room = kMaxPending - pending_.size();
    const size_t kept = static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room;
    pending_.insert(pending_.end(), chunk, chunk + kept);
    return kData;
}

bool WirelessSetup::next(uint16_t& id, std::vector<uint8_t>& body) {
    while (pending_.size() >= kHeaderSize) {
        const size_t length = bigEndian16(&pending_[0]);
        id = static_cast<uint16_t>(bigEndian16(&pending_[2]));
        if (length > kMaxBody || id == 0 || id > kLastId) {
            printf("[Wireless] out of sync (hdr %02x %02x %02x %02x): dropping 1 byte\n", pending_[0], pending_[1],
                   pending_[2], pending_[3]);
            pending_.erase(pending_.begin());
            continue;
        }
        if (pending_.size() < kHeaderSize + length) {
            return false;
        }
        body.assign(pending_.begin() + kHeaderSize, pending_.begin() + kHeaderSize + length);
        pending_.erase(pending_.begin(), pending_.begin() + kHeaderSize + length);
        spoke_ = true;
        return true;
    }
    return false;
}

WirelessSetup::Step WirelessSetup::handle(uint16_t id, const std::vector<uint8_t>& body, Progress& progress) {
    printf("[Wireless] RX msg id=%u len=%u\n", id, static_cast<unsigned>(body.size()));
    if (!body.empty()) {
        printBytes("   payload", &body[0], body.size(), 64);
        printFields(body);
    }
    uint64_t status = 0;
    switch (id) {
        case kInfoRequest:
            printf("[Wireless] TX WifiInfoResponse ssid='%s' bssid='%s' security=%d\n", ap_.ssid.c_str(),
                   ap_.bssid.c_str(), ap_.security);
            if (!write(infoResponse(ap_))) {
                return kWriteFailed;
            }
            progress.infoSent = true;
            break;
        case kStartResponse: {
            // The documented status is field 4; the phones seen send only field 3 = 0 (`18 00`). With a field 4,
            // field 3 is the port, and a non-zero one is no refusal.
            uint64_t status3 = 0;
            const bool has4 = field(body, 4, status);
            const bool has3 = field(body, 3, status3);
            if (has4) {
                printf("[Wireless] WifiStartResponse status field(4)=%llu\n", static_cast<unsigned long long>(status));
            } else if (has3) {
                printf("[Wireless] WifiStartResponse status field(3)=%llu\n", static_cast<unsigned long long>(status3));
            } else {
                printf("[Wireless] WifiStartResponse (no status field)\n");
            }
            if (!progress.infoSent) {
                ++progress.startAnswers;
            }
            if (has4 ? status == 0 : (has3 && status3 == 0)) {
                progress.startAccepted = true;
            }
            break;
        }
        case kConnectStatus: {
            // 0 joined; 12 also seen from phones that went on. Without a status, joined.
            const bool known = field(body, 1, status) || field(body, 2, status);
            printf("[Wireless] WifiConnectStatus status=%llu\n", static_cast<unsigned long long>(known ? status : 0));
            return !known || status == 0 || status == 12 ? kJoined : kGoOn;
        }
        case kStartRequest:
            // The phone started the exchange itself: confirmed with our address and port.
            printf(
                "[Wireless] phone sent its own WifiStartRequest: answering WifiStartResponse ip=%s port=%u status=0\n",
                ap_.ip.c_str(), static_cast<unsigned>(port_));
            if (!write(startResponse(ap_, port_))) {
                return kWriteFailed;
            }
            break;
        case kPingRequest:
            printf("[Wireless] ping -> pong\n");
            if (!write(wireMessage(kPingResponse, body))) {
                return kWriteFailed;
            }
            break;
        case kVersionResponse:
            printf("[Wireless] WifiVersionResponse from phone (the phone speaks the newer WPP)\n");
            break;
        default:
            printf("[Wireless] ignoring unknown msg id=%u\n", id);
            break;
    }
    return kGoOn;
}

bool WirelessSetup::handshake(int timeoutMs) {
    printf("[Wireless] handshake mode 0 (legacy)\n");
    pending_.clear();
    foreign_ = spoke_ = false;
    // Listen before talking: a BMW app link that already sent its startup magic gets nothing written into it. Android
    // Auto waits up to 10 s for our first message.
    if (peekMs_ > 0 && fill(peekMs_) == kGone) {
        printf(foreign_ ? "[Wireless] the BMW app spoke first on this link: nothing written, leaving it to the car\n"
                        : "[Wireless] link closed before we wrote anything\n");
        return false;
    }
    const std::vector<uint8_t> request = startRequest(ap_, port_);
    printBytes("TX WifiStartRequest", &request[0], request.size(), 64);
    if (!write(request)) {
        printf("[Wireless] write WifiStartRequest failed: %s\n", strerror(errno));
        return false;
    }
    Progress progress = {false, 0, false};
    uint64_t startedMs = monotonicMs();
    uint64_t infoSentMs = 0;
    int resends = 0;
    for (;;) {
        uint16_t id;
        std::vector<uint8_t> body;
        while (next(id, body)) {
            const Step step = handle(id, body, progress);
            if (step == kWriteFailed) {
                return false;
            }
            if (progress.infoSent && infoSentMs == 0) {
                infoSentMs = monotonicMs();
            }
            if (step == kJoined) {
                return true;
            }
        }
        const Fill filled = fill(kWaitMs);
        if (filled == kGone) {
            if (foreign_) {
                return false;
            }
            // The phone closes Bluetooth on its way to Wi-Fi, once it accepted the start. Closed right after the
            // access point without that is the warm-boot failure: the phone never joins.
            if (progress.startAccepted && progress.infoSent) {
                printf("[Wireless] phone closed Bluetooth after accepting the Wi-Fi start (joining)\n");
                return true;
            }
            printf(progress.infoSent ? "[Wireless] phone closed Bluetooth before accepting the Wi-Fi start\n"
                                     : "[Wireless] phone closed the Bluetooth link before the AP info was exchanged\n");
            return false;
        }
        if (filled == kData) {
            continue;
        }
        const uint64_t now = monotonicMs();
        if (progress.infoSent) {
            if (now - infoSentMs > kJoinWaitMs) {
                printf(progress.startAccepted
                           ? "[Wireless] phone accepted the Wi-Fi start; waiting for its TCP connection\n"
                           : "[Wireless] phone sent no WifiStartResponse after WifiInfoResponse\n");
                return progress.startAccepted;
            }
        } else if (now - startedMs > static_cast<uint64_t>(timeoutMs)) {
            if (progress.startAnswers && resends < kResends) {
                ++resends;
                printf("[Wireless] no WifiInfoRequest after msg 7; re-sending WifiStartRequest (%d/%d)\n", resends,
                       kResends);
                printBytes("TX WifiStartRequest", &request[0], request.size(), 64);
                if (!write(request)) {
                    return false;
                }
                startedMs = now;
                continue;
            }
            printf("[Wireless] timeout waiting for the phone\n");
            return false;
        }
    }
}

bool WirelessSetup::listen(int waitMs) {
    const Fill filled = fill(waitMs);
    uint16_t id;
    std::vector<uint8_t> body;
    while (next(id, body)) {
        ++heard_;
        if (id == kPingRequest) {
            printf("[Wireless] ping while Android Auto runs -> pong\n");
            if (!write(wireMessage(kPingResponse, body))) {
                return false;
            }
            continue;
        }
        printf("[Wireless] while Android Auto runs the phone sent msg id=%u len=%u: not answered\n", id,
               static_cast<unsigned>(body.size()));
        if (!body.empty()) {
            printBytes("   payload", &body[0], body.size(), 64);
        }
    }
    return filled != kGone;
}

bool WirelessSetup::serve(int waitMs) {
    const Fill filled = fill(waitMs);
    Progress progress = {true, 0, false};
    uint16_t id;
    std::vector<uint8_t> body;
    while (next(id, body)) {
        if (handle(id, body, progress) == kWriteFailed) {
            return false;
        }
    }
    return filled != kGone;
}

}  // namespace aa
