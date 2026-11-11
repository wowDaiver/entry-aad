// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The Wi-Fi link's figures and their log line (wifi_link.hpp).
#include "wifi_link.hpp"

#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "evo_log.hpp"
#include "mutex_lock.hpp"

namespace evo {

namespace {

const uint32_t kPeriodMs = 10000;
const uint32_t kStartGraceMs = 5000;  // a connection whose session never started is dropped after this
const unsigned kRadioTries = 3;       // nl80211 silent that many times in a row: not asked again for this connection
const long kAnswerUs = 200000;        // the main loop never waits longer for the kernel

size_t align4(size_t n) {
    return (n + 3) & ~static_cast<size_t>(3);
}

// ---- netlink attributes: length (2 bytes, header included), type (2 bytes), value, padded to 4 bytes ----------------
struct Attr {
    unsigned type;
    const uint8_t* data;
    size_t len;
};

bool nextAttr(const uint8_t*& p, size_t& left, Attr& a) {
    nlattr header;
    if (left < sizeof(header)) {
        return false;
    }
    memcpy(&header, p, sizeof(header));
    if (header.nla_len < sizeof(header) || header.nla_len > left) {
        return false;
    }
    a.type = header.nla_type & NLA_TYPE_MASK;
    a.data = p + sizeof(header);
    a.len = header.nla_len - sizeof(header);
    const size_t step = align4(header.nla_len) > left ? left : align4(header.nla_len);
    p += step;
    left -= step;
    return true;
}

int64_t number(const Attr& a) {
    if (a.len == 1) {
        return a.data[0];
    }
    if (a.len == 2) {
        uint16_t v;
        memcpy(&v, a.data, sizeof(v));
        return v;
    }
    if (a.len == 4) {
        uint32_t v;
        memcpy(&v, a.data, sizeof(v));
        return v;
    }
    return kNoFigure;
}

// A signal level: one signed byte, in dBm.
int64_t level(const Attr& a) {
    return a.len == 1 ? static_cast<int8_t>(a.data[0]) : kNoFigure;
}

int64_t bitrate(const Attr& nested) {
    const uint8_t* p = nested.data;
    size_t left = nested.len;
    Attr a;
    while (nextAttr(p, left, a)) {
        if (a.type == NL80211_RATE_INFO_BITRATE && a.len == 2) {
            return number(a);
        }
    }
    return kNoFigure;
}

void stationFigures(const Attr& nested, WifiStation& out) {
    const uint8_t* p = nested.data;
    size_t left = nested.len;
    Attr a;
    while (nextAttr(p, left, a)) {
        switch (a.type) {
            case NL80211_STA_INFO_SIGNAL:
                out.signalDbm = level(a);
                break;
            case NL80211_STA_INFO_SIGNAL_AVG:
                out.signalAvgDbm = level(a);
                break;
            case NL80211_STA_INFO_TX_BITRATE:
                out.txRate = bitrate(a);
                break;
            case NL80211_STA_INFO_RX_BITRATE:
                out.rxRate = bitrate(a);
                break;
            case NL80211_STA_INFO_TX_PACKETS:
                out.txFrames = a.len == 4 ? number(a) : kNoFigure;
                break;
            case NL80211_STA_INFO_TX_RETRIES:
                out.txRetries = a.len == 4 ? number(a) : kNoFigure;
                break;
            case NL80211_STA_INFO_TX_FAILED:
                out.txFailed = a.len == 4 ? number(a) : kNoFigure;
                break;
            case NL80211_STA_INFO_CONNECTED_TIME:
                out.connectedS = a.len == 4 ? number(a) : kNoFigure;
                break;
            default:
                break;
        }
    }
}

// ---- one generic netlink conversation with the kernel, opened for a sample and closed after it ----------------------
class Nl80211 {
public:
    Nl80211() : fd_(-1), family_(0), interface_(0), sequence_(0) {}
    ~Nl80211() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    bool open(const char* interface) {
        interface_ = if_nametoindex(interface);
        if (!interface_) {
            return false;
        }
        fd_ = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
        if (fd_ < 0) {
            return false;
        }
        sockaddr_nl self;
        memset(&self, 0, sizeof(self));
        self.nl_family = AF_NETLINK;
        timeval limit;
        limit.tv_sec = 0;
        limit.tv_usec = kAnswerUs;
        if (bind(fd_, reinterpret_cast<sockaddr*>(&self), sizeof(self)) != 0 ||
            setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit)) != 0) {
            return false;
        }
        static const char name[] = "nl80211";
        return ask(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, false, CTRL_ATTR_FAMILY_NAME, name, sizeof(name), &onFamily,
                   &family_) &&
               family_ != 0;
    }

    bool stations(std::vector<WifiStation>& out) {
        out.clear();
        return ask(family_, NL80211_CMD_GET_STATION, true, NL80211_ATTR_IFINDEX, &interface_, sizeof(interface_),
                   &onStation, &out);
    }

    bool frequency(unsigned& mhz) {
        mhz = 0;
        return ask(family_, NL80211_CMD_GET_INTERFACE, false, NL80211_ATTR_IFINDEX, &interface_, sizeof(interface_),
                   &onInterface, &mhz) &&
               mhz != 0;
    }

private:
    typedef void (*OnAnswer)(const uint8_t* attrs, size_t len, void* user);

    static void onFamily(const uint8_t* attrs, size_t len, void* user) {
        Attr a;
        while (nextAttr(attrs, len, a)) {
            if (a.type == CTRL_ATTR_FAMILY_ID && a.len == 2) {
                *static_cast<uint16_t*>(user) = static_cast<uint16_t>(number(a));
            }
        }
    }
    static void onStation(const uint8_t* attrs, size_t len, void* user) {
        WifiStation station;
        if (parseWifiStation(attrs, len, station)) {
            static_cast<std::vector<WifiStation>*>(user)->push_back(station);
        }
    }
    static void onInterface(const uint8_t* attrs, size_t len, void* user) {
        parseWifiFrequency(attrs, len, *static_cast<unsigned*>(user));
    }

    // One request carrying one attribute, then its answers: one message, or with dump every message up to "done".
    bool ask(uint16_t type, uint8_t command, bool dump, uint16_t attrType, const void* attr, size_t attrLen,
             OnAnswer on, void* user) {
        nlmsghdr header;
        genlmsghdr generic;
        nlattr attribute;
        std::vector<uint8_t> request(sizeof(header) + sizeof(generic) + sizeof(attribute) + align4(attrLen), 0);
        memset(&header, 0, sizeof(header));
        header.nlmsg_len = static_cast<uint32_t>(request.size());
        header.nlmsg_type = type;
        header.nlmsg_flags = static_cast<uint16_t>(NLM_F_REQUEST | (dump ? NLM_F_DUMP : 0));
        header.nlmsg_seq = ++sequence_;
        memset(&generic, 0, sizeof(generic));
        generic.cmd = command;
        generic.version = 1;
        attribute.nla_type = attrType;
        attribute.nla_len = static_cast<uint16_t>(sizeof(attribute) + attrLen);
        memcpy(&request[0], &header, sizeof(header));
        memcpy(&request[sizeof(header)], &generic, sizeof(generic));
        memcpy(&request[sizeof(header) + sizeof(generic)], &attribute, sizeof(attribute));
        memcpy(&request[sizeof(header) + sizeof(generic) + sizeof(attribute)], attr, attrLen);
        sockaddr_nl kernel;
        memset(&kernel, 0, sizeof(kernel));
        kernel.nl_family = AF_NETLINK;
        if (sendto(fd_, &request[0], request.size(), 0, reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) !=
            static_cast<ssize_t>(request.size())) {
            return false;
        }
        uint8_t answer[8192];
        for (;;) {
            const ssize_t got = recv(fd_, answer, sizeof(answer), 0);
            if (got <= 0) {
                return false;  // nothing within kAnswerUs
            }
            const uint8_t* p = answer;
            size_t left = static_cast<size_t>(got);
            bool answered = false;
            while (left >= sizeof(header)) {
                memcpy(&header, p, sizeof(header));
                if (header.nlmsg_len < sizeof(header) || header.nlmsg_len > left) {
                    return false;
                }
                const uint8_t* body = p + sizeof(header);
                const size_t bodyLen = header.nlmsg_len - sizeof(header);
                const size_t step = align4(header.nlmsg_len) > left ? left : align4(header.nlmsg_len);
                p += step;
                left -= step;
                if (header.nlmsg_seq != sequence_) {
                    continue;
                }
                if (header.nlmsg_type == NLMSG_DONE) {
                    return true;
                }
                if (header.nlmsg_type == NLMSG_ERROR) {
                    return false;  // no acknowledgement was asked for: this is a refusal
                }
                if (bodyLen >= sizeof(generic)) {
                    on(body + sizeof(generic), bodyLen - sizeof(generic), user);
                    answered = true;
                }
            }
            if (!dump && answered) {
                return true;
            }
        }
    }

    int fd_;
    uint16_t family_;
    uint32_t interface_;
    uint32_t sequence_;
};

// The hardware address of the phone at the other end of the connection, from the kernel's neighbour table.
bool phoneAddress(int fd, const char* interface, uint8_t mac[6]) {
    sockaddr_in peer;
    socklen_t len = sizeof(peer);
    memset(&peer, 0, sizeof(peer));
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &len) != 0 || peer.sin_family != AF_INET) {
        return false;
    }
    arpreq request;
    memset(&request, 0, sizeof(request));
    memcpy(&request.arp_pa, &peer, sizeof(peer));
    strncpy(request.arp_dev, interface, sizeof(request.arp_dev) - 1);
    if (ioctl(fd, SIOCGARP, &request) != 0 || !(request.arp_flags & ATF_COM)) {
        return false;
    }
    memcpy(mac, request.arp_ha.sa_data, 6);
    return true;
}

void add(std::string& line, const char* separator, const char* fmt, long long a, long long b = 0) {
    char text[96];
    snprintf(text, sizeof(text), fmt, a, b);
    if (!line.empty()) {
        line += separator;
    }
    line += text;
}

// "812 frames sent, 35 retried, 0 lost" from the figures known, counted from the earlier ones when given.
std::string frames(const WifiStation& now, const WifiStation* since) {
    std::string s;
    if (now.txFrames != kNoFigure) {
        add(s, ", ", "%lld frames sent", now.txFrames - (since ? since->txFrames : 0));
    }
    if (now.txRetries != kNoFigure) {
        add(s, ", ", "%lld retried", now.txRetries - (since ? since->txRetries : 0));
    }
    if (now.txFailed != kNoFigure) {
        add(s, ", ", "%lld lost", now.txFailed - (since ? since->txFailed : 0));
    }
    return s;
}

// The same phone, still on the Wi-Fi since the earlier sample: its counters can be subtracted.
bool continues(const WifiStation& now, const WifiStation& before) {
    return !memcmp(now.mac, before.mac, sizeof(now.mac)) &&
           (now.txFrames == kNoFigure) == (before.txFrames == kNoFigure) && now.txFrames >= before.txFrames &&
           (now.txRetries == kNoFigure) == (before.txRetries == kNoFigure) && now.txRetries >= before.txRetries &&
           (now.txFailed == kNoFigure) == (before.txFailed == kNoFigure) && now.txFailed >= before.txFailed &&
           (now.connectedS == kNoFigure || before.connectedS == kNoFigure || now.connectedS >= before.connectedS);
}

}  // namespace

WifiStation::WifiStation()
    : signalDbm(kNoFigure),
      signalAvgDbm(kNoFigure),
      txRate(kNoFigure),
      rxRate(kNoFigure),
      txFrames(kNoFigure),
      txRetries(kNoFigure),
      txFailed(kNoFigure),
      connectedS(kNoFigure) {
    memset(mac, 0, sizeof(mac));
}

WifiSample::WifiSample() : atMs(0), radio(false), mhz(0), stations(0), hasStation(false), hasTcp(false) {
    tcp.roundTripUs = 0;
    tcp.resent = 0;
}

bool parseWifiStation(const uint8_t* attrs, size_t len, WifiStation& out) {
    out = WifiStation();
    bool named = false;
    Attr a;
    while (nextAttr(attrs, len, a)) {
        if (a.type == NL80211_ATTR_MAC && a.len == sizeof(out.mac)) {
            memcpy(out.mac, a.data, sizeof(out.mac));
            named = true;
        } else if (a.type == NL80211_ATTR_STA_INFO) {
            stationFigures(a, out);
        }
    }
    return named;
}

bool parseWifiFrequency(const uint8_t* attrs, size_t len, unsigned& mhz) {
    Attr a;
    while (nextAttr(attrs, len, a)) {
        if (a.type == NL80211_ATTR_WIPHY_FREQ && a.len == 4) {
            mhz = static_cast<unsigned>(number(a));
            return true;
        }
    }
    return false;
}

bool readTcpLink(int fd, TcpLink& out) {
    tcp_info info;
    socklen_t len = sizeof(info);
    memset(&info, 0, sizeof(info));
    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0) {
        return false;
    }
    out.roundTripUs = info.tcpi_rtt;
    out.resent = info.tcpi_total_retrans;
    return true;
}

std::string describeWifiLink(const WifiSample& now, const WifiSample* before) {
    std::string line;
    if (now.radio && now.mhz) {
        add(line, "; ", "%lld MHz", now.mhz);
    }
    if (now.radio && !now.hasStation) {
        add(line, "; ", "phone not found among the %lld station(s) of the access point", now.stations);
    }
    if (now.hasStation) {
        const WifiStation& phone = now.station;
        if (phone.signalDbm != kNoFigure && phone.signalAvgDbm != kNoFigure) {
            add(line, "; ", "signal %lld dBm (average %lld)", phone.signalDbm, phone.signalAvgDbm);
        } else if (phone.signalDbm != kNoFigure) {
            add(line, "; ", "signal %lld dBm", phone.signalDbm);
        }
        if (phone.txRate != kNoFigure) {
            add(line, "; ", "to the phone %lld.%lld Mbit/s", phone.txRate / 10, phone.txRate % 10);
        }
        if (phone.rxRate != kNoFigure) {
            add(line, "; ", "from the phone %lld.%lld Mbit/s", phone.rxRate / 10, phone.rxRate % 10);
        }
        const bool same = before && before->hasStation && continues(phone, before->station);
        const std::string counted = frames(phone, same ? &before->station : nullptr);
        if (before && before->hasStation && !same) {
            line += line.empty() ? "" : "; ";
            line += "the phone joined the Wi-Fi again";
        }
        if (!counted.empty()) {
            if (same) {
                add(line, "; ", "in %lld s: ", (now.atMs - before->atMs + 500) / 1000);
            } else {
                line += line.empty() ? "" : "; ";
                line += "since it joined: ";
            }
            line += counted;
        }
        if (phone.connectedS != kNoFigure) {
            add(line, "; ", "on the Wi-Fi for %lld s", phone.connectedS);
        }
    }
    if (now.hasTcp) {
        add(line, "; ", "TCP round trip %lld.%lld ms", now.tcp.roundTripUs / 1000, now.tcp.roundTripUs % 1000 / 100);
        if (before && before->hasTcp && now.tcp.resent >= before->tcp.resent) {
            add(line, ", ", "%lld segment(s) sent again", now.tcp.resent - before->tcp.resent);
        } else {
            add(line, ", ", "%lld segment(s) sent again since it opened", now.tcp.resent);
        }
    }
    return line;
}

WifiLinkLog::WifiLinkLog()
    : fd_(-1),
      first_(false),
      seenActive_(false),
      hasBefore_(false),
      radioGone_(false),
      nextMs_(0),
      sinceMs_(0),
      radioFailures_(0) {
    pthread_mutex_init(&lock_, nullptr);
}

WifiLinkLog::~WifiLinkLog() {
    forget();
    pthread_mutex_destroy(&lock_);
}

void WifiLinkLog::enable(const char* interface) {
    aa::MutexLock lock(lock_);
    interface_ = interface;
}

void WifiLinkLog::forget() {
    if (fd_ >= 0) {
        close(fd_);
    }
    fd_ = -1;
}

void WifiLinkLog::connected(int fd) {
    aa::MutexLock lock(lock_);
    if (interface_.empty()) {
        return;
    }
    forget();
    fd_ = fcntl(fd, F_DUPFD_CLOEXEC, 0);  // never inherited by the programs the service starts
    first_ = true;
    seenActive_ = hasBefore_ = radioGone_ = false;
    radioFailures_ = 0;
}

void WifiLinkLog::turn(uint32_t nowMs, bool sessionActive) {
    aa::MutexLock lock(lock_);
    if (fd_ < 0) {
        return;
    }
    if (first_) {
        first_ = false;
        sinceMs_ = nextMs_ = nowMs;
    }
    if (!sessionActive) {
        // Over, or the connection came in a moment before its session started.
        if (seenActive_ || nowMs - sinceMs_ >= kStartGraceMs) {
            forget();
        }
        return;
    }
    seenActive_ = true;
    if (static_cast<int32_t>(nowMs - nextMs_) < 0) {
        return;
    }
    nextMs_ = nowMs + kPeriodMs;
    sample(nowMs);
}

void WifiLinkLog::sample(uint32_t nowMs) {
    WifiSample now;
    now.atMs = nowMs;
    now.hasTcp = readTcpLink(fd_, now.tcp);
    if (!radioGone_) {
        Nl80211 radio;
        std::vector<WifiStation> all;
        if (radio.open(interface_.c_str()) && radio.stations(all)) {
            radioFailures_ = 0;
            now.radio = true;
            now.stations = static_cast<unsigned>(all.size());
            radio.frequency(now.mhz);
            uint8_t mac[6];
            if (phoneAddress(fd_, interface_.c_str(), mac)) {
                for (size_t i = 0; i < all.size(); ++i) {
                    if (!memcmp(all[i].mac, mac, sizeof(mac))) {
                        now.station = all[i];
                        now.hasStation = true;
                    }
                }
            } else if (all.size() == 1) {
                now.station = all[0];  // the only phone on the access point
                now.hasStation = true;
            }
        } else if (++radioFailures_ >= kRadioTries) {
            radioGone_ = true;
            ELOG("WifiLink", "no Wi-Fi figures from the kernel for %s (nl80211, errno %d): TCP figures only",
                 interface_.c_str(), errno);
        }
    }
    const std::string line = describeWifiLink(now, hasBefore_ ? &before_ : nullptr);
    if (!line.empty()) {
        ELOG("WifiLink", "%s", line.c_str());
    }
    before_ = now;
    hasBefore_ = true;
}

}  // namespace evo
