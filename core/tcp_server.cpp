// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The phone's TCP listener: polled once a second, its accepted sockets tuned, filtered and handed to the session.
#include "tcp_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace aa {
namespace {

const int kBacklog = 5;
const unsigned kFailuresBeforeRelisten = 3;
const int kRelistenAttempts = 5;
const uint32_t kSubnetMask = 0xffffff00u;  // the hotspot's /24
// The car's own CarPlay marks its screen stream this way (Apple's SocketSetQoS in Cinemo's libNmeCarPlay.so). The
// kernel turns it into the Wi-Fi video category (WMM AC_VI, 802.1d priority 4), which gets on the air before best
// effort. Everything the car sends on this connection goes that way: video acks, input, TCP acks of the phone's video.
const int kWifiVideoTos = 0x80;

bool setOption(int fd, int level, int name, int value) {
    return setsockopt(fd, level, name, &value, sizeof(value)) == 0;
}

// Small frames (acks, input) must not wait for Nagle or delayed acks, nor queue behind ordinary traffic on the Wi-Fi;
// a phone gone from Wi-Fi must end in an error rather than an endless recv(): keepalive after 10 s idle, 3 probes 3 s
// apart.
void tune(int fd) {
    if (!setOption(fd, IPPROTO_IP, IP_TOS, kWifiVideoTos)) {
        printf("[TcpServer] could not give the connection the Wi-Fi video priority: %s\n", strerror(errno));
    }
    setOption(fd, IPPROTO_TCP, TCP_NODELAY, 1);
    setOption(fd, SOL_SOCKET, SO_KEEPALIVE, 1);
#ifdef TCP_KEEPIDLE
    setOption(fd, IPPROTO_TCP, TCP_KEEPIDLE, 10);
#endif
#ifdef TCP_KEEPINTVL
    setOption(fd, IPPROTO_TCP, TCP_KEEPINTVL, 3);
#endif
#ifdef TCP_KEEPCNT
    setOption(fd, IPPROTO_TCP, TCP_KEEPCNT, 3);
#endif
}

sockaddr_in anyAddress(uint16_t port) {
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = INADDR_ANY;
    return address;
}

}  // namespace

TcpServer::TcpServer(uint16_t port)
    : port_(port),
      callback_(nullptr),
      user_(nullptr),
      thread_(0),
      running_(false),
      busy_(false),
      listener_(-1),
      failures_(0),
      lastError_(0),
      accepted_(0) {}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::setConnectionCallback(ClientConnectedCallback callback, void* user_data) {
    callback_ = callback;
    user_ = user_data;
}

// The hotspot address when it can be bound, so that nobody on another interface can open a session. It is often not
// up yet at start: then every interface, and run() filters the phones.
int TcpServer::bindListener() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[TcpServer] socket creation failed");
        return -1;
    }
    setOption(fd, SOL_SOCKET, SO_REUSEADDR, 1);
    sockaddr_in address = anyAddress(port_);
    bool bound = false;
    if (!hotspot_.empty()) {
        bound = inet_aton(hotspot_.c_str(), &address.sin_addr) &&
                bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        if (bound) {
            printf("[TcpServer] listening on %s:%u\n", hotspot_.c_str(), static_cast<unsigned>(port_));
        } else {
            printf("[TcpServer] could not bind %s:%u (%s); listening on all interfaces\n", hotspot_.c_str(),
                   static_cast<unsigned>(port_), strerror(errno));
            address = anyAddress(port_);
        }
    }
    if (!bound && bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        perror("[TcpServer] bind failed");
        close(fd);
        return -1;
    }
    if (::listen(fd, kBacklog) < 0) {
        perror("[TcpServer] listen failed");
        close(fd);
        return -1;
    }
    return fd;
}

// Listener thread only.
void TcpServer::relisten(const char* why) {
    const int old = listener_;
    if (old >= 0) {
        close(old);
    }
    listener_ = -1;
    for (int attempt = 0; attempt < kRelistenAttempts && running_ && listener_ < 0; ++attempt) {
        listener_ = bindListener();
        if (listener_ < 0) {
            sleep(1);
        }
    }
    printf(
        "[TcpServer] listener on port %u made again (%s): fd %d -> %d, %u accepted so far, %u socket errors (last: "
        "%s)\n",
        port_, why, old, listener_, accepted_, failures_, lastError_ ? strerror(lastError_) : "none");
    failures_ = 0;
    lastError_ = 0;
}

bool TcpServer::start() {
    if (running_) {
        return true;
    }
    listener_ = bindListener();
    if (listener_ < 0) {
        return false;
    }
    running_ = true;
    busy_ = false;
    if (pthread_create(&thread_, nullptr, &TcpServer::threadMain, this) != 0) {
        perror("[TcpServer] accept thread");
        running_ = false;  // no thread for stop() to join
        close(listener_);
        listener_ = -1;
        return false;
    }
    printf("[TcpServer] Android Auto listener active on port %u\n", port_);
    return true;
}

void TcpServer::stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    if (thread_) {
        pthread_join(thread_, nullptr);
        thread_ = 0;
    }
    if (listener_ >= 0) {
        close(listener_);
        listener_ = -1;
    }
    printf("[TcpServer] Listener stopped.\n");
}

void* TcpServer::threadMain(void* self) {
    static_cast<TcpServer*>(self)->run();
    return nullptr;
}

bool TcpServer::fromHotspot(uint32_t address) const {
    const in_addr_t hotspot = hotspot_.empty() ? INADDR_NONE : inet_addr(hotspot_.c_str());
    return hotspot == INADDR_NONE || (ntohl(address) & kSubnetMask) == (ntohl(hotspot) & kSubnetMask);
}

// select() with a 1 s timeout rather than a blocking accept(), so that stop() never waits for a phone.
void TcpServer::run() {
    while (running_) {
        const int fd = listener_;
        if (fd < 0) {
            // A listener that could not be made again: keep trying rather than end the thread.
            sleep(1);
            if (running_) {
                relisten("no listening socket");
            }
            continue;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        timeval wait;
        wait.tv_sec = 1;
        wait.tv_usec = 0;
        const int ready = select(fd + 1, &readable, nullptr, nullptr, &wait);
        if (ready <= 0) {
            if (ready < 0 && errno != EINTR && running_) {
                lastError_ = errno;
                if (++failures_ == 1) {
                    printf("[TcpServer] select() on the listener failed: %s\n", strerror(errno));
                }
                // A listener that keeps failing (the car's Wi-Fi switched off under it) would never accept again.
                if (failures_ >= kFailuresBeforeRelisten) {
                    relisten("listener socket keeps failing");
                } else {
                    sleep(1);
                }
            }
            continue;
        }

        sockaddr_in peer;
        socklen_t peerSize = sizeof(peer);
        const int phone = accept(fd, reinterpret_cast<sockaddr*>(&peer), &peerSize);
        if (phone < 0) {
            if (!running_) {
                break;
            }
            if (errno != EINTR && errno != ECONNABORTED && errno != EAGAIN) {
                lastError_ = errno;
                perror("[TcpServer] accept failed");
                if (++failures_ >= kFailuresBeforeRelisten) {
                    relisten("accept() keeps failing");
                } else {
                    sleep(1);  // no busy loop on a lasting error (EMFILE...)
                }
            }
            continue;
        }
        failures_ = 0;
        ++accepted_;
        tune(phone);

        char from[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, from, sizeof(from));
        printf("[TcpServer] Incoming Android Auto connection from %s:%u (fd: %d)!\n", from, ntohs(peer.sin_port),
               phone);
        if (busy_) {
            printf("[TcpServer] session already active, refusing %s\n", from);
            close(phone);
            continue;
        }
        if (!fromHotspot(peer.sin_addr.s_addr)) {
            printf("[TcpServer] refusing %s (not on the Android Auto Wi-Fi)\n", from);
            close(phone);
            continue;
        }
        busy_ = true;
        if (callback_) {
            callback_(phone, user_);
        } else {
            close(phone);
        }
    }
}

}  // namespace aa
