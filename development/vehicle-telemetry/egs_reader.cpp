// SPDX-License-Identifier: GPL-3.0-or-later
#include "egs_reader.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef TELEMETRY_EGS_CHECK
#include <stdio.h>
#endif
namespace telemetry {
EgsReader::EgsReader(unsigned ecu, bool expanded)
    : phase_(Off),
      fd_(-1),
      identified_(false),
      result_(false),
      valid_(false),
      value_(0),
      deadline_(0),
      due_(0),
      last_(0),
      did_(0),
      frames_(0),
      alive_pending_(0),
      ecu_(ecu),
      cursor_(0),
      target_(ecu),
      identity_mask_(0),
      expanded_(expanded),
      retry_delay_(1),
      used_(0),
      out_used_(0),
      out_size_(0) {
    memset(next_due_, 0, sizeof(next_due_));
    memset(&reply_, 0, sizeof(reply_));
}
EgsReader::~EgsReader() {
    stop();
}
void EgsReader::close_socket() {
    if (fd_ >= 0) {
        close(fd_);
    }
    fd_ = -1;
    used_ = out_used_ = out_size_ = 0;
    alive_pending_ = 0;
    identified_ = false;
    identity_mask_ = 0;
}
void EgsReader::stop() {
    close_socket();
    phase_ = Off;
    result_ = false;
}
void EgsReader::start(double now) {
    stop();
    unsigned count = 0;
    diagnostic_sources(ecu_, expanded_, count);
    if (!count || !isfinite(now) || now < 0) {
        return;
    }
    last_ = due_ = now;
    cursor_ = 0;
    retry_delay_ = 1;
    for (unsigned i = 0; i < MaxDiagnosticSources; ++i) {
        next_due_[i] = now;
    }
    phase_ = Backoff;
}
bool EgsReader::take(double& v, bool& ok) {
    if (!result_) {
        return false;
    }
    result_ = false;
    v = value_;
    ok = valid_;
    return true;
}
bool EgsReader::take_record(DiagnosticReply& reply) {
    if (!result_) {
        return false;
    }
    result_ = false;
    reply = reply_;
    return true;
}
void EgsReader::fail(double now, unsigned reason) {
#ifdef TELEMETRY_EGS_CHECK
    fprintf(stderr, "EGS failure phase=%d errno=%d at=%.3f deadline=%.3f bytes=%zu\n", phase_, errno, now, deadline_,
            used_);
#endif
    close_socket();
    phase_ = Backoff;
    if (!expanded_ || reason == IdentityFailure) {
        retry_delay_ = 30;
    }
    due_ = now + retry_delay_;
    retry_delay_ = retry_delay_ < 15 ? retry_delay_ * 2 : 30;
    result_ = true;
    valid_ = false;
    reply_.ecu = ecu_;
    reply_.did = reply_.size = 0;
    reply_.valid = false;
    reply_.failure = reason;
}
bool EgsReader::temperature(const unsigned char* p, size_t n, double& value) {
    // GSZFB 6000: unsigned byte - 40 C. Reject reserved/high values.
    if (n != 1 || !p || p[0] > 245) {
        return false;
    }
    value = static_cast<double>(p[0]) - 40;
    return true;
}
void EgsReader::connect_socket(double now) {
#if defined(TELEMETRY_HOST_ABI_STUB) && !defined(TELEMETRY_EGS_LOOPBACK_TEST)
    fail(now);
    return;  // Host controller/factory tests must never contact a vehicle.
#endif
    close_socket();
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        fail(now);
        return;
    }
    if (fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd_, F_SETFL, O_NONBLOCK) < 0) {
        fail(now);
        return;
    }
    sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(6801);
#ifdef TELEMETRY_EGS_LOOPBACK_TEST
    // Separate host test/probe binary only. No configurable production endpoint.
    inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);
    inet_pton(AF_INET, "127.0.0.1", &peer.sin_addr);
#elif defined(TELEMETRY_EGS_INTERNAL_CHECK)
    inet_pton(AF_INET, "160.48.199.119", &local.sin_addr);
    inet_pton(AF_INET, "160.48.199.16", &peer.sin_addr);
#else
    // This vehicle's observed ENET addresses. Fail closed if the route changes.
    inet_pton(AF_INET, "169.254.1.0", &local.sin_addr);
    inet_pton(AF_INET, "169.254.76.216", &peer.sin_addr);
#endif
    if (bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        fail(now);
        return;
    }
#ifdef SO_NOSIGPIPE
    int on = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) < 0) {
        fail(now);
        return;
    }
#endif
    int r = connect(fd_, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    if (r < 0 && errno != EINPROGRESS) {
        fail(now);
        return;
    }
    phase_ = Connecting;
    deadline_ = now + 3;
}
void EgsReader::request(unsigned did, double now) {
    if ((did != 0xf150 && (!identified_ || record_index(did) < 0)) || out_size_ || alive_pending_) {
        fail(now);
        return;
    }
    target_ = ecu_ ? ecu_ : (did == 0xf150 ? ((identity_mask_ & 1) ? 0x18 : 0x12) : diagnostic_ecu(did));
    const bool obd = did == 0x0104;
    const unsigned char frame[] = {0,
                                   0,
                                   0,
                                   static_cast<unsigned char>(obd ? 4 : 5),
                                   0,
                                   1,
                                   0xf4,
                                   static_cast<unsigned char>(target_),
                                   static_cast<unsigned char>(obd ? 1 : 0x22),
                                   static_cast<unsigned char>(obd ? 4 : did >> 8),
                                   static_cast<unsigned char>(did)};
    out_size_ = obd ? 10 : 11;
    memcpy(output_, frame, out_size_);
    out_used_ = 0;
    did_ = did;
    frames_ = 0;
    phase_ = Sending;
    deadline_ = now + 3;
}
int EgsReader::record_index(unsigned did) const {
    unsigned count = 0;
    const DiagnosticSource* sources = diagnostic_sources(ecu_, expanded_, count);
    for (unsigned i = 0; i < count; ++i) {
        if (sources[i].did == did) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
void EgsReader::complete(bool valid, const unsigned char* data, unsigned size, double now) {
    int index = record_index(did_);
    if (index < 0) {
        fail(now);
        return;
    }
    unsigned count = 0;
    const DiagnosticSource* sources = diagnostic_sources(ecu_, expanded_, count);
    if (size != sources[index].size || size > sizeof(reply_.data)) {
        valid = false;
    }
    reply_.ecu = target_;
    reply_.did = did_;
    reply_.size = valid ? size : 0;
    reply_.valid = valid;
    reply_.failure = valid ? NoDiagnosticFailure : ProtocolFailure;
    if (valid) {
        memcpy(reply_.data, data, size);
    }
    if (did_ == 0x6000 && valid) {
        valid = temperature(data, size, value_);
    }
    valid_ = valid;
    reply_.valid = valid;
    result_ = true;
    if (valid) {
        retry_delay_ = 1;
    }
    next_due_[index] = now + (valid ? sources[index].interval : 30);
    cursor_ = (index + 1) % count;
    phase_ = Idle;
    due_ = now;
    if (alive_pending_) {
        deadline_ = now + 3;
    }
}
bool EgsReader::schedule(double now) {
    if (phase_ != Idle || now < due_ || out_size_ || alive_pending_) {
        return false;
    }
    if (!identified_) {
        request(0xf150, now);
        return true;
    }
    unsigned count = 0;
    const DiagnosticSource* sources = diagnostic_sources(ecu_, expanded_, count);
    for (unsigned step = 0; step < count; ++step) {
        unsigned i = (cursor_ + step) % count;
        if (now >= next_due_[i]) {
            request(sources[i].did, now);
            return true;
        }
    }
    return false;
}
bool EgsReader::parse(double now) {
    // Each transaction is bounded by 32 frames and one absolute deadline.
    while (used_ >= 6) {
        unsigned length =
            (unsigned(input_[0]) << 24) | (unsigned(input_[1]) << 16) | (unsigned(input_[2]) << 8) | input_[3];
        unsigned control = (unsigned(input_[4]) << 8) | input_[5];
        if (length > sizeof(input_) - 6) {
            fail(now, ProtocolFailure);
            return false;
        }
        if (used_ < length + 6) {
            return true;
        }
        if (++frames_ > 32) {
            fail(now, ProtocolFailure);
            return false;
        }
        unsigned char body[506];
        memcpy(body, input_ + 6, length);
        used_ -= length + 6;
        memmove(input_, input_ + length + 6, used_);
        if (control == 0x12) {
            // HSFZ transport alive ACK only. Never a UDS tester-present.
            // Keep ACKs independent of the diagnostic phase: one TCP read can
            // also contain the completed identification/temperature response.
            if (alive_pending_ >= 32) {
                fail(now);
                return false;
            }
            if (phase_ == Idle && !alive_pending_ && !out_size_) {
                deadline_ = now + 3;
            }
            ++alive_pending_;
            continue;
        }
        if (control >= 0x40) {
            fail(now);
            return false;
        }
        if (control != 1 || length < 3 || body[0] != target_ || body[1] != 0xf4) {
            continue;
        }
        if (phase_ != Awaiting) {
            fail(now, ProtocolFailure);
            return false;
        }
        if (length == 5 && body[2] == 0x7f && body[3] == (did_ == 0x0104 ? 1 : 0x22)) {
            if (body[4] == 0x78) {
                continue;
            }
            if (expanded_ && identified_ && did_ != 0xf150) {
                complete(false, 0, 0, now);
                continue;
            }
            fail(now);
            return false;
        }
        unsigned prefix = did_ == 0x0104 ? 4 : 5;
        if (did_ == 0x0104) {
            if (length < 4 || body[2] != 0x41 || body[3] != 4) {
                continue;
            }
        } else if (length < 5 || body[2] != 0x62 || body[3] != (did_ >> 8) || body[4] != (did_ & 255)) {
            continue;
        }
        if (did_ == 0xf150) {
            if (!diagnostic_identity(target_, body + 5, length - 5)) {
                fail(now, IdentityFailure);
                return false;
            }
            identity_mask_ |= target_ == 0x12 ? 1 : 2;
            identified_ = ecu_ != 0 || identity_mask_ == 3;
            // Finish parsing this TCP read and drain transport ACKs before
            // issuing the temperature request. Do not overwrite queued output.
            phase_ = Idle;
            due_ = now;
            if (alive_pending_) {
                deadline_ = now + 3;
            }
            continue;
        }
        if (!identified_) {
            fail(now);
            return false;
        }
        if (!expanded_ && !temperature(body + 5, length - 5, value_)) {
            fail(now);
            return false;
        }
        complete(true, body + prefix, length - prefix, now);
        // Trailing transport ACK/alive frames belong to the same byte stream.
        // A diagnostic reply while idle is still rejected by the phase gate.
    }
    return true;
}
bool EgsReader::flush_output(double now) {
    if (!out_size_) {
        return true;
    }
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    ssize_t n = send(fd_, output_ + out_used_, out_size_ - out_used_, flags);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            fail(now);
        }
        return false;
    }
    if (!n) {
        fail(now);
        return false;
    }
    out_used_ += static_cast<size_t>(n);
    if (out_used_ < out_size_) {
        return false;
    }
    out_used_ = out_size_ = 0;
    if (phase_ == Sending) {
        phase_ = Awaiting;
    }
    return true;
}
void EgsReader::poll(double now) {
    if (phase_ == Off) {
        return;
    }
    if (!isfinite(now) || now < last_) {
        stop();
        return;
    }
    last_ = now;
    if (phase_ == Backoff) {
        if (now >= due_) {
            connect_socket(now);
        }
        return;
    }
    if ((phase_ != Idle || out_size_ || alive_pending_) && now >= deadline_) {
        fail(now, ResponseTimeout);
        return;
    }
    bool issued = false;
    if (phase_ == Connecting) {
        pollfd p = {fd_, POLLOUT, 0};
        int r = ::poll(&p, 1, 0);
        if (r < 0) {
            if (errno != EINTR) {
                fail(now);
            }
            return;
        }
        if (!r) {
            return;
        }
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error) {
            if (error) {
                errno = error;
            }
            fail(now);
            return;
        }
        request(0xf150, now);
        issued = true;
    }
    if (!flush_output(now)) {
        return;
    }
    if (alive_pending_) {
        const unsigned char ack[] = {0, 0, 0, 2, 0, 0x13, 0, 0xf4};
        memcpy(output_, ack, sizeof(ack));
        out_size_ = sizeof(ack);
        out_used_ = 0;
        --alive_pending_;
        if (!flush_output(now)) {
            return;
        }
        if (alive_pending_) {
            return;  // Bounded work: at most one new alive ACK per tick.
        }
    }
    if (schedule(now)) {
        issued = true;
    }
    if (!flush_output(now)) {
        return;
    }
    ssize_t n = recv(fd_, input_ + used_, sizeof(input_) - used_, 0);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            fail(now);
        }
        return;
    }
    if (!n) {
        fail(now);
        return;
    }
    used_ += static_cast<size_t>(n);
    if (!parse(now)) {
        return;
    }
    // Unified connection: after finishing one reply, send the next request
    // without an extra idle tick. Never pipeline or issue >1 request per call.
    if (ecu_ == 0 && !issued && schedule(now)) {
        flush_output(now);
    }
}
}  // namespace telemetry
