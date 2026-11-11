// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The USB bridge's thread (see usb_bridge.hpp): one loop, never blocked on one side while the other has work.
#include "usb_bridge.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <vector>

#include "aa_frame.hpp"
#include "evo_log.hpp"

namespace evo {
namespace {

// The phone writes at most 16 KiB at a time; a multiple of every packet size, so a transfer never overflows.
const size_t kReadSize = 16384;
// A frame the phone does not take: it is gone, or its Android Auto stopped reading.
const uint32_t kWriteLimitMs = 5000;
const int kPollMs = 500;
const uint32_t kCancelMs = 300;
const char kSessionClosed[] = "session closed";
const char kStopped[] = "stopped";

// The size of the Android Auto frame at the start of `bytes`, or 0 while it is not whole.
size_t wholeFrame(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < aa::frame::kHeaderSize) {
        return 0;
    }
    const aa::FrameHeader header = aa::decodeFrameHeader(&bytes[0]);
    const size_t size =
        aa::frame::kHeaderSize + (header.announcesSize() ? aa::frame::kTotalSizeSize : 0) + header.length;
    return bytes.size() >= size ? size : 0;
}

}  // namespace

UsbBridge::UsbBridge(UsbDevice* device, const AccessoryEndpoints& endpoints, int socketFd)
    : device_(device),
      endpoints_(endpoints),
      socket_(socketFd),
      thread_(0),
      started_(false),
      stop_(false),
      finished_(false),
      phoneLeft_(false),
      fromPhone_(0),
      toPhone_(0) {
    wake_[0] = wake_[1] = -1;
}

UsbBridge::~UsbBridge() {
    stop_ = true;
    if (started_) {
        const char byte = 0;
        if (write(wake_[1], &byte, 1) < 0) {
            // The thread still sees stop_ at its next turn.
        }
        pthread_join(thread_, nullptr);
    } else {
        delete device_;
        if (socket_ >= 0) {
            close(socket_);
        }
    }
    if (wake_[0] >= 0) {
        close(wake_[0]);
        close(wake_[1]);
    }
}

bool UsbBridge::start() {
    if (pipe2(wake_, O_CLOEXEC | O_NONBLOCK) != 0) {
        wake_[0] = wake_[1] = -1;
        return false;
    }
    started_ = pthread_create(&thread_, nullptr, &UsbBridge::threadMain, this) == 0;
    return started_;
}

void* UsbBridge::threadMain(void* self) {
    static_cast<UsbBridge*>(self)->run();
    return nullptr;
}

void UsbBridge::run() {
    ELOG("UsbBridge", "phone on USB: in 0x%02x, out 0x%02x, packets of %u", endpoints_.in, endpoints_.out,
         endpoints_.outPacket);
    const char* why = carry();
    ELOG("UsbBridge", "ended: %s; %llu bytes from the phone, %llu to it", why,
         static_cast<unsigned long long>(fromPhone_.load()), static_cast<unsigned long long>(toPhone_.load()));
    if (device_) {
        device_->releaseInterface(0);
        delete device_;
        device_ = nullptr;
    }
    shutdown(socket_, SHUT_RDWR);
    close(socket_);
    socket_ = -1;
    finished_ = true;
}

const char* UsbBridge::carry() {
    UsbTransfer in, out, empty;
    in.endpoint = endpoints_.in;
    in.data.resize(kReadSize);
    out.endpoint = empty.endpoint = endpoints_.out;
    bool reading = false;
    unsigned writing = 0;  // transfers out in flight: the frame, and the empty packet after it when it needs one
    uint32_t writingSince = 0;
    std::vector<uint8_t> forSession;  // read from the phone, not yet taken by the socket
    size_t forSessionAt = 0;
    std::vector<uint8_t> forPhone;  // read from the socket: the frame being gathered, and what follows it
    const char* why = nullptr;
    while (!why && !stop_) {
        // One read in flight, unless the session has not yet taken the last one: the phone then waits, as on TCP.
        if (!reading && forSessionAt == forSession.size()) {
            if (!device_->submit(&in)) {
                why = "read refused";
                break;
            }
            reading = true;
        }
        // One Android Auto frame per transfer, as head units send them.
        const size_t frame = writing ? 0 : wholeFrame(forPhone);
        if (frame) {
            out.data.assign(forPhone.begin(), forPhone.begin() + frame);
            forPhone.erase(forPhone.begin(), forPhone.begin() + frame);
            if (!device_->submit(&out)) {
                why = "write refused";
                break;
            }
            writing = 1;
            writingSince = monoMs();
            // A transfer that fills its last packet does not end for the phone: an empty packet ends it.
            if (frame % endpoints_.outPacket == 0) {
                if (!device_->submit(&empty)) {
                    why = "write refused";
                    break;
                }
                writing = 2;
            }
        }
        bool reaped = false;
        while (UsbTransfer* transfer = device_->reap()) {
            reaped = true;
            if (transfer == &in) {
                reading = false;
                if (transfer->status) {
                    why = "read failed";
                } else {
                    forSession.assign(in.data.begin(), in.data.begin() + in.done);
                    forSessionAt = 0;
                    fromPhone_ += in.done;
                }
            } else {
                --writing;
                if (transfer->status || transfer->done != transfer->data.size()) {
                    why = "write failed";
                } else {
                    toPhone_ += transfer->done;
                }
            }
        }
        if (device_->gone()) {
            why = "phone unplugged";
        }
        if (why || reaped) {
            continue;
        }
        if (forSessionAt < forSession.size()) {
            const ssize_t n =
                send(socket_, &forSession[forSessionAt], forSession.size() - forSessionAt, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n > 0) {
                forSessionAt += static_cast<size_t>(n);
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                why = kSessionClosed;
                continue;
            }
        }
        pollfd fds[3];
        fds[0].fd = device_->waitFd();
        fds[0].events = reading || writing ? device_->waitEvents() : 0;
        fds[1].fd = socket_;
        fds[1].events = (writing ? 0 : POLLIN) | (forSessionAt < forSession.size() ? POLLOUT : 0);
        fds[2].fd = wake_[0];
        fds[2].events = POLLIN;
        fds[0].revents = fds[1].revents = fds[2].revents = 0;
        if (poll(fds, 3, kPollMs) < 0 && errno != EINTR) {
            why = "poll failed";
            continue;
        }
        if (fds[1].revents & POLLIN) {
            uint8_t bytes[kReadSize];
            const ssize_t n = recv(socket_, bytes, sizeof(bytes), MSG_DONTWAIT);
            if (n > 0) {
                forPhone.insert(forPhone.end(), bytes, bytes + n);
            } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                why = kSessionClosed;
            }
        } else if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            why = kSessionClosed;
        }
        if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) && !(fds[0].revents & device_->waitEvents())) {
            why = "phone unplugged";
        }
        if (!why && writing && monoMs() - writingSince >= kWriteLimitMs) {
            why = "the phone takes no data";
        }
    }
    if (!why) {
        why = kStopped;
    }
    phoneLeft_ = why != kStopped && why != kSessionClosed;
    // The transfers in flight point into this function's buffers: get them back before leaving.
    if (reading) {
        device_->cancel(&in);
    }
    if (writing) {
        device_->cancel(&out);
        device_->cancel(&empty);
    }
    const uint32_t begin = monoMs();
    while ((reading || writing) && !device_->gone() && monoMs() - begin < kCancelMs) {
        UsbTransfer* transfer = device_->reap();
        if (!transfer) {
            usleep(5000);
        } else if (transfer == &in) {
            reading = false;
        } else {
            --writing;
        }
    }
    if (reading || writing) {
        // Still held by the kernel: closing the device takes them back, and it must come before the buffers go.
        device_->releaseInterface(0);
        delete device_;
        device_ = nullptr;
    }
    return why;
}

}  // namespace evo
