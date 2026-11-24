// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Reads the car's access point settings from hostapd (or its ap.conf) and wlan0's address.
#include "evo_ap.hpp"

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "evo_log.hpp"
#include "evo_process.hpp"

namespace evo {
// One hostapd_cli command on the car's access point (wlan0); false when it could not run or ended in error.
static bool callHostapd(const std::vector<std::string>& command, std::string& out, int timeoutMs) {
    std::vector<std::string> args = {"-p", "/var/run/hostapd", "-i", "wlan0"};
    args.insert(args.end(), command.begin(), command.end());
    return runCommand("/usr/sbin/hostapd_cli", args, out, timeoutMs);
}
static bool queryHostapd(std::string& out) {
    return callHostapd(std::vector<std::string>{"get_config"}, out, 2000);
}
// RELOAD derives the passphrase's key again: more time than a query.
static const int kChangeTimeoutMs = 5000;
static HostapdStart currentHostapdStart() {
    HostapdStart start;
    struct stat st;
    if (stat("/var/run/hostapd/wlan0", &st) == 0) {
        start.device = st.st_dev;
        start.inode = st.st_ino;
        start.known = true;
    }
    return start;
}
// The wireless loop's thread only.
UapsdOffer withdrawAccessPointUapsd() {
    static HostapdStart rebuiltFor;  // the hostapd start whose beacon was rebuilt
    const HostapdStart now = currentHostapdStart();
    const UapsdOffer offer =
        withdrawUapsd([](const std::vector<std::string>& command,
                         std::string& reply) { return callHostapd(command, reply, kChangeTimeoutMs); },
                      now.same(rebuiltFor));
    if (offer == UapsdOffer::kWithdrawn) {
        rebuiltFor = now;
    }
    return offer;
}
static bool macAddress(const std::string& value) {
    if (value.size() != 17) {
        return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (i % 3 == 2) {
            if (c != ':') {
                return false;
            }
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}
bool loadAccessPoint(aa::ApInfo& out, std::string& source) {
    std::string text;
    aa::ApInfo ap;
    if (queryHostapd(text) && parseHostapdConfig(text, ap)) {
        source = "hostapd_cli/wlan0";
    } else {
        const char* const file = "/tmp/wland/ap.conf";
        std::ifstream in(file, std::ios::binary);
        text.clear();
        char buf[1024];
        while (in.read(buf, sizeof(buf)) || in.gcount()) {
            text.append(buf, static_cast<size_t>(in.gcount()));
            if (text.size() > 65536) {
                return false;
            }
        }
        if (!parseHostapdConfig(text, ap)) {
            return false;
        }
        source = file;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }
    ifreq request;
    memset(&request, 0, sizeof(request));
    strcpy(request.ifr_name, "wlan0");
    bool haveIp = ioctl(fd, SIOCGIFADDR, &request) == 0;
    if (haveIp) {
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&request.ifr_addr)->sin_addr, ip, sizeof(ip))) {
            ap.ip = ip;
        }
    }
    if (ap.bssid.empty() && ioctl(fd, SIOCGIFHWADDR, &request) == 0) {
        const unsigned char* b = reinterpret_cast<unsigned char*>(request.ifr_hwaddr.sa_data);
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
        ap.bssid = mac;
    }
    close(fd);
    in_addr address;
    if (!haveIp || inet_pton(AF_INET, ap.ip.c_str(), &address) != 1 || !macAddress(ap.bssid)) {
        return false;
    }
    out = ap;
    return true;
}
}  // namespace evo
