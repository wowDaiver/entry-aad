// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The phone listener on loopback: one session at a time, the hotspot's /24 only, the accepted socket's options.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

#include "tcp_server.hpp"

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                            \
        }                                                       \
    } while (0)

namespace {

std::atomic<int> g_handed(-1);  // the socket the server handed over
std::atomic<int> g_calls(0);

void onPhone(int fd, void*) {
    g_handed = fd;
    ++g_calls;
}

int dial(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in to = sockaddr_in();
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    return fd;
}

// The server closed this connection without handing it over.
bool turnedAway(int fd) {
    pollfd p = {fd, POLLIN, 0};
    char byte;
    const bool closed = poll(&p, 1, 3000) == 1 && read(fd, &byte, 1) == 0;
    close(fd);
    return closed;
}

bool handedOver(int calls) {
    for (int i = 0; i < 300 && g_calls < calls; ++i) {
        usleep(10000);
    }
    return g_calls == calls;
}

int option(int fd, int level, int name) {
    int value = -1;
    socklen_t size = sizeof(value);
    getsockopt(fd, level, name, &value, &size);
    return value;
}

}  // namespace

int main() {
    const uint16_t port = static_cast<uint16_t>(40000 + getpid() % 20000);
    {
        aa::TcpServer server(port);
        server.setHotspotAddress("127.0.0.1");
        server.setConnectionCallback(onPhone, nullptr);
        CHECK(server.start() && server.start());
        const int phone = dial(port);
        CHECK(phone >= 0 && handedOver(1));
        // Small frames go at once, a vanished phone is noticed: TCP_NODELAY, keepalive 10 s then 3 probes 3 s apart.
        CHECK(option(g_handed, IPPROTO_TCP, TCP_NODELAY) == 1 && option(g_handed, SOL_SOCKET, SO_KEEPALIVE) == 1);
        CHECK(option(g_handed, IPPROTO_TCP, TCP_KEEPIDLE) == 10 && option(g_handed, IPPROTO_TCP, TCP_KEEPINTVL) == 3);
        CHECK(option(g_handed, IPPROTO_TCP, TCP_KEEPCNT) == 3);
        // What the car sends goes in the Wi-Fi video category, as its CarPlay marks its screen stream.
        CHECK(option(g_handed, IPPROTO_IP, IP_TOS) == 0x80);
        // One session at a time, until the session says it ended.
        CHECK(turnedAway(dial(port)) && g_calls == 1);
        server.notifySessionEnded();
        const int again = dial(port);
        CHECK(again >= 0 && handedOver(2));
        server.notifySessionEnded();
        server.markSessionActive();
        CHECK(turnedAway(dial(port)) && g_calls == 2);
        close(phone);
        close(again);
        close(g_handed);
        server.stop();
        server.stop();
        CHECK(dial(port) < 0);
    }
    {
        // A hotspot address this machine does not have: listening on every interface, but loopback is not its /24.
        aa::TcpServer server(static_cast<uint16_t>(port + 1));
        server.setHotspotAddress("10.255.255.1");
        server.setConnectionCallback(onPhone, nullptr);
        CHECK(server.start());
        CHECK(turnedAway(dial(static_cast<uint16_t>(port + 1))) && g_calls == 2);
    }
    {
        // No hotspot: no filter. No callback: the connection is closed.
        aa::TcpServer open(static_cast<uint16_t>(port + 2));
        open.setConnectionCallback(onPhone, nullptr);
        CHECK(open.start());
        const int phone = dial(static_cast<uint16_t>(port + 2));
        CHECK(phone >= 0 && handedOver(3));
        close(phone);
        close(g_handed);
        aa::TcpServer silent(static_cast<uint16_t>(port + 3));
        CHECK(silent.start());
        CHECK(turnedAway(dial(static_cast<uint16_t>(port + 3))) && g_calls == 3);
    }
    puts("PASS phone listener: one session at a time, hotspot /24 only, socket options, start and stop twice");
    return 0;
}
