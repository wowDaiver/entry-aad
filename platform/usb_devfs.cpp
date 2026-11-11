// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// A USB device through the kernel's usbfs (/dev/bus/usb): no library, and no kernel driver is ever detached.
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>

#include "usb_device.hpp"

namespace evo {
namespace {

class DevfsDevice : public UsbDevice {
public:
    explicit DevfsDevice(int fd) : fd_(fd), gone_(false) {}

    ~DevfsDevice() override {
        // Closing ends every transfer still in flight before the kernel returns: their requests can go after it.
        close(fd_);
        for (std::set<usbdevfs_urb*>::iterator it = flying_.begin(); it != flying_.end(); ++it) {
            delete *it;
        }
    }

    int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t size,
                unsigned timeoutMs) override {
        usbdevfs_ctrltransfer transfer;
        memset(&transfer, 0, sizeof(transfer));
        transfer.bRequestType = type;
        transfer.bRequest = request;
        transfer.wValue = value;
        transfer.wIndex = index;
        transfer.wLength = size;
        transfer.timeout = timeoutMs;
        transfer.data = data;
        const int n = ioctl(fd_, USBDEVFS_CONTROL, &transfer);
        return n < 0 ? -errno : n;
    }

    bool claimInterface(unsigned number) override { return ioctl(fd_, USBDEVFS_CLAIMINTERFACE, &number) == 0; }

    void releaseInterface(unsigned number) override { ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &number); }

    bool reset() override { return ioctl(fd_, USBDEVFS_RESET, 0) == 0; }

    bool submit(UsbTransfer* transfer) override {
        usbdevfs_urb* urb = new usbdevfs_urb;
        memset(urb, 0, sizeof(*urb));
        urb->type = USBDEVFS_URB_TYPE_BULK;
        urb->endpoint = transfer->endpoint;
        urb->buffer = transfer->data.empty() ? nullptr : &transfer->data[0];
        urb->buffer_length = static_cast<int>(transfer->data.size());
        urb->usercontext = transfer;
        if (ioctl(fd_, USBDEVFS_SUBMITURB, urb) != 0) {
            gone_ = gone_ || errno == ENODEV;
            delete urb;
            return false;
        }
        transfer->request = urb;
        flying_.insert(urb);
        return true;
    }

    UsbTransfer* reap() override {
        usbdevfs_urb* urb = nullptr;
        if (ioctl(fd_, USBDEVFS_REAPURBNDELAY, &urb) != 0) {
            gone_ = gone_ || errno == ENODEV;
            return nullptr;
        }
        if (!urb) {
            return nullptr;
        }
        UsbTransfer* transfer = static_cast<UsbTransfer*>(urb->usercontext);
        transfer->status = urb->status;
        transfer->done = urb->actual_length > 0 ? static_cast<size_t>(urb->actual_length) : 0;
        transfer->request = nullptr;
        flying_.erase(urb);
        delete urb;
        return transfer;
    }

    void cancel(UsbTransfer* transfer) override {
        if (transfer->request) {
            ioctl(fd_, USBDEVFS_DISCARDURB, transfer->request);
        }
    }

    bool gone() const override { return gone_; }
    int waitFd() const override { return fd_; }
    short waitEvents() const override { return POLLOUT; }  // usbfs: writable when a transfer can be reaped

private:
    int fd_;
    bool gone_;
    std::set<usbdevfs_urb*> flying_;
};

}  // namespace

UsbDevice* openUsbDevice(unsigned bus, unsigned address) {
    char path[48];
    snprintf(path, sizeof(path), "/dev/bus/usb/%03u/%03u", bus, address);
    const int fd = open(path, O_RDWR | O_CLOEXEC);
    return fd < 0 ? nullptr : new DevfsDevice(fd);
}

}  // namespace evo
