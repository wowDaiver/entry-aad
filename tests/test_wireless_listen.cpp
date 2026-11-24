// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// core/wireless_setup: a Bluetooth link that comes back while Android Auto runs is only listened to.

// The link is a SOCK_SEQPACKET socketpair: like the car's RFCOMM device, it keeps each message whole.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "wireless_setup.hpp"

namespace {

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

// What the head unit wrote to the phone, or -1 when it wrote nothing.
ssize_t written(int phone, uint8_t* to, size_t cap) {
    return recv(phone, to, cap, MSG_DONTWAIT);
}

}  // namespace

int main() {
    int link[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, link) != 0) {
        puts("FAIL socketpair");
        return 1;
    }
    const int car = link[0], phone = link[1];
    aa::ApInfo ap;
    ap.ssid = "car";
    ap.key = "secret";
    ap.bssid = "00:11:22:33:44:55";
    ap.ip = "172.16.222.49";
    ap.security = 8;
    aa::WirelessSetup setup(car, ap, 50000, 300);
    uint8_t got[64];

    // Nothing from the phone: nothing is sent first, the link stays.
    CHECK(setup.listen(50) && setup.heard() == 0);
    CHECK(written(phone, got, sizeof(got)) < 0);

    // A ping: echoed back as a pong (message 9), body kept.
    const uint8_t ping[] = {0x00, 0x02, 0x00, 0x08, 0x08, 0x2a};
    CHECK(send(phone, ping, sizeof(ping), 0) == static_cast<ssize_t>(sizeof(ping)));
    CHECK(setup.listen(200) && setup.heard() == 1);
    const uint8_t pong[] = {0x00, 0x02, 0x00, 0x09, 0x08, 0x2a};
    CHECK(written(phone, got, sizeof(got)) == static_cast<ssize_t>(sizeof(pong)) && !memcmp(got, pong, sizeof(pong)));

    // The phone's own start request, an info request: counted, written to the log, not answered. An answer would
    // send the phone to a new TCP connection while its session runs.
    const uint8_t start[] = {0x00, 0x00, 0x00, 0x01};
    const uint8_t info[] = {0x00, 0x00, 0x00, 0x02};
    CHECK(send(phone, start, sizeof(start), 0) == static_cast<ssize_t>(sizeof(start)));
    CHECK(setup.listen(200) && setup.heard() == 2);
    CHECK(send(phone, info, sizeof(info), 0) == static_cast<ssize_t>(sizeof(info)));
    CHECK(setup.listen(200) && setup.heard() == 3);
    CHECK(written(phone, got, sizeof(got)) < 0);

    // The phone closes the link.
    close(phone);
    CHECK(!setup.listen(200));
    close(car);

    if (g_failures) {
        printf("FAIL wireless listen: %d checks failed\n", g_failures);
        return 1;
    }
    puts(
        "PASS Bluetooth link back during a session: nothing sent first, ping answered, requests left alone, closing "
        "seen");
    return 0;
}
