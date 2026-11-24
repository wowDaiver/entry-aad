// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The address of the control channel between the iDrive app and the Android Auto service.

// A Unix datagram socket in Linux's abstract namespace: a name, no file and no port. The channel was a UDP port on
// 127.0.0.1 before, 52790, inside the range the head unit hands out to any program that asks for a free port
// (net.ipv4.ip_local_port_range = 49152 65535): one start-up of the car, another program was given that port, and
// the service, finding it taken, concluded that a service was already running. An abstract name is taken only by a
// program that asks for that very name, and the kernel frees it when the process ends.
#ifndef ENTRY_AAD_PLATFORM_CONTROL_ADDRESS_HPP
#define ENTRY_AAD_PLATFORM_CONTROL_ADDRESS_HPP

#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace evo {

// Fills the abstract address of a name and returns its length; a name too long for the address is cut.
inline socklen_t controlAddress(const char* name, struct sockaddr_un& out) {
    memset(&out, 0, sizeof(out));
    out.sun_family = AF_UNIX;
    size_t n = name ? strlen(name) : 0;
    if (n > sizeof(out.sun_path) - 1) {
        n = sizeof(out.sun_path) - 1;
    }
    if (n) {
        memcpy(out.sun_path + 1, name, n);  // sun_path[0] stays 0: abstract namespace
    }
    return static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

// Gives a datagram socket an address of its own, picked by the kernel, so that a reply can come back to it.
inline bool controlReplyAddress(int fd) {
    struct sockaddr_un own;
    memset(&own, 0, sizeof(own));
    own.sun_family = AF_UNIX;
    return bind(fd, reinterpret_cast<struct sockaddr*>(&own), sizeof(sa_family_t)) == 0;
}

// ---- the iDrive app's side: neither call ever waits on the service, they run on iDrive's threads -----------------

// One command for the service holding the name. False when none holds it (the kernel refuses at once) or when its
// queue is full.
inline bool sendControl(const char* name, const char* message) {
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    struct sockaddr_un to;
    const socklen_t length = controlAddress(name, to);
    const size_t size = strlen(message);
    const bool sent = sendto(fd, message, size, MSG_DONTWAIT, reinterpret_cast<struct sockaddr*>(&to), length) ==
                      static_cast<ssize_t>(size);
    close(fd);
    return sent;
}

// True when a service answers "pong" to a "ping" within waitMs. With no service on the name the answer is "no" at
// once, not after the wait.
inline bool controlAnswers(const char* name, int waitMs) {
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    struct sockaddr_un to;
    const socklen_t length = controlAddress(name, to);
    bool answered = false;
    if (controlReplyAddress(fd) && connect(fd, reinterpret_cast<struct sockaddr*>(&to), length) == 0 &&
        send(fd, "ping", 4, MSG_DONTWAIT) == 4) {
        struct pollfd wait;
        wait.fd = fd;
        wait.events = POLLIN;
        wait.revents = 0;
        if (poll(&wait, 1, waitMs) > 0) {
            char reply[8];
            const ssize_t n = recv(fd, reply, sizeof(reply), MSG_DONTWAIT);
            answered = n >= 4 && memcmp(reply, "pong", 4) == 0;
        }
    }
    close(fd);
    return answered;
}

}  // namespace evo

#endif
