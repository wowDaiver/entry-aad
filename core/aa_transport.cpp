// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Socket implementation of the session byte stream, also used by the original wireless mode.
#include "aa_transport.hpp"

#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace aa {
namespace {
class SocketTransport : public Transport {
public:
    explicit SocketTransport(int fd) : fd_(fd) {
        timeval limit = {5, 0};
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
    }
    ~SocketTransport() override {
        shutdown(fd_, SHUT_RDWR);
        close(fd_);
    }
    ssize_t read(uint8_t* data, size_t size, int wakeFd, bool& woke) override {
        woke = false;
        if (fd_ < 0 || fd_ >= FD_SETSIZE || wakeFd >= FD_SETSIZE) {
            return -1;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        int highest = fd_;
        if (wakeFd >= 0) {
            FD_SET(wakeFd, &fds);
            if (wakeFd > highest) {
                highest = wakeFd;
            }
        }
        timeval wait = {1, 0};
        const int ready = select(highest + 1, &fds, nullptr, nullptr, &wait);
        if (ready < 0) {
            return errno == EINTR ? 0 : -1;
        }
        woke = ready > 0 && wakeFd >= 0 && FD_ISSET(wakeFd, &fds);
        if (!ready || !FD_ISSET(fd_, &fds)) {
            return 0;
        }
        const ssize_t n = recv(fd_, data, size, 0);
        if (n < 0 && errno == EINTR) {
            return 0;
        }
        return n > 0 ? n : -1;
    }
    ssize_t write(const uint8_t* data, size_t size) override {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;  // process guards ignore SIGPIPE on the head unit
#endif
        const ssize_t n = send(fd_, data, size, flags);
        return n < 0 && errno == EINTR ? 0 : n;
    }
    void cancel() override { shutdown(fd_, SHUT_RDWR); }
    const char* name() const override { return "TCP"; }

private:
    int fd_;
};
}  // namespace
Transport* socketTransport(int fd) {
    return new SocketTransport(fd);
}
}  // namespace aa
