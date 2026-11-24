// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Control channel between the iDrive app and the Android Auto service (see evo_control.hpp).
#include "evo_control.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "control_address.hpp"
#include "evo_log.hpp"

namespace evo {

bool parseControlCommand(const char* data, size_t len, ControlCommand& out) {
    if (!data || !len || len > 64) {
        return false;
    }
    char buf[65];
    memcpy(buf, data, len);
    buf[len] = 0;
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
        buf[--len] = 0;
    }
    char verb[16] = "";
    int arg = 0;
    char extra[8] = "";
    int n = sscanf(buf, "%15s %d %7s", verb, &arg, extra);
    if (n < 1 || n > 2) {
        return false;
    }
    static const char* const known[] = {"ping", "show",     "hide",      "quit", "gear", "await",
                                        "pdc",  "daynight", "drivemode", "call", "popup"};
    bool ok = false;
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (!strcmp(verb, known[i])) {
            ok = true;
        }
    }
    if (!ok) {
        return false;
    }
    const bool needsArg = !strcmp(verb, "gear") || !strcmp(verb, "pdc") || !strcmp(verb, "daynight") ||
                          !strcmp(verb, "call") || !strcmp(verb, "popup");
    if (needsArg != (n == 2) && strcmp(verb, "drivemode")) {
        return false;
    }
    out.verb = verb;
    out.arg = arg;
    out.hasArg = n == 2;
    return true;
}

EvoControl::EvoControl() : fd_(-1), started_(false), running_(false), lastWriteMs_(0), cb_(nullptr), user_(nullptr) {}
EvoControl::~EvoControl() {
    stop();
}

bool EvoControl::start(const char* name, const char* stateFile, Callback cb, void* user) {
    if (started_) {
        return true;
    }
    fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        ELOG("Control", "no control channel: socket: %s", strerror(errno));
        return false;
    }
    sockaddr_un a;
    const socklen_t length = controlAddress(name, a);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&a), length) != 0) {
        // Only a program that asks for this very name can hold it: "in use" is a service of ours, still running.
        ELOG("Control", "control channel \"%s\" not available (%s)%s", name, strerror(errno),
             errno == EADDRINUSE ? ": a service already runs" : "");
        close(fd_);
        fd_ = -1;
        return false;
    }
    stateFile_ = stateFile ? stateFile : "";
    cb_ = cb;
    user_ = user;
    running_ = true;
    if (pthread_create(&thread_, nullptr, threadMain, this) != 0) {
        running_ = false;
        close(fd_);
        fd_ = -1;
        return false;
    }
    started_ = true;
    ELOG("Control", "iDrive app commands on the control channel \"%s\"; state in %s", name, stateFile_.c_str());
    return true;
}

void EvoControl::stop() {
    if (!started_) {
        return;
    }
    running_ = false;
    pthread_join(thread_, nullptr);
    started_ = false;
    close(fd_);
    fd_ = -1;
    if (!stateFile_.empty()) {
        unlink(stateFile_.c_str());
    }
}

void EvoControl::writeState(const char* state, int overlay) {
    if (stateFile_.empty()) {
        return;
    }
    char line[64];
    snprintf(line, sizeof(line), "%s %d %d\n", state, static_cast<int>(getpid()), overlay);
    const uint32_t now = monoMs();
    if (last_ == line && now - lastWriteMs_ < 5000) {
        return;
    }
    std::string tmp = stateFile_ + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return;
    }
    ssize_t w = write(fd, line, strlen(line));
    (void)w;
    close(fd);
    if (rename(tmp.c_str(), stateFile_.c_str()) == 0) {
        if (last_ != line) {
            ELOG("Control", "state -> %s", line);
        }
        last_ = line;
        lastWriteMs_ = now;
    }
}

void* EvoControl::threadMain(void* self) {
    static_cast<EvoControl*>(self)->run();
    return nullptr;
}

void EvoControl::run() {
    char buf[96];
    while (running_) {
        struct pollfd p;
        p.fd = fd_;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 200) <= 0) {
            continue;
        }
        sockaddr_un from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(fd_, buf, sizeof(buf), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) {
            continue;
        }
        ControlCommand c;
        if (!parseControlCommand(buf, static_cast<size_t>(n), c)) {
            ELOG("Control", "command ignored (%d B)", (int)n);
            continue;
        }
        if (c.verb == "ping") {
            // A sender with no address of its own cannot be answered; never wait on a sender that does not read.
            if (fl > sizeof(sa_family_t) && fl <= sizeof(from)) {
                sendto(fd_, "pong", 4, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), fl);
            }
            continue;
        }
        if (c.hasArg) {
            ELOG("Control", "app command: %s %d", c.verb.c_str(), c.arg);
        } else {
            ELOG("Control", "app command: %s", c.verb.c_str());
        }
        if (cb_) {
            cb_(c, user_);
        }
    }
}

}  // namespace evo
