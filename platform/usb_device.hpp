// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// One USB device as the service uses it: control requests, one interface, bulk transfers that complete later.
//
// The head unit's implementation talks to the kernel directly (usbfs, usb_devfs.cpp); the tests have their own.
#ifndef ENTRY_AAD_PLATFORM_USB_DEVICE_HPP
#define ENTRY_AAD_PLATFORM_USB_DEVICE_HPP

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace evo {

// A bulk transfer. Filled by its owner, handed to submit(), given back by reap() once the kernel is done with it.
struct UsbTransfer {
    uint8_t endpoint;           // 0x80 set: from the device
    std::vector<uint8_t> data;  // out: the bytes to send (may be empty); in: the room, kept at its size
    size_t done;                // bytes transferred
    int status;                 // 0, or a negative errno
    void* request;              // the implementation's own
    UsbTransfer() : endpoint(0), done(0), status(0), request(nullptr) {}
};

class UsbDevice {
public:
    virtual ~UsbDevice() {}
    // A request on endpoint 0: the bytes transferred, or a negative errno.
    virtual int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t size,
                        unsigned timeoutMs) = 0;
    // False with errno set: EBUSY when another program holds the interface. Nobody is ever pushed out of it.
    virtual bool claimInterface(unsigned number) = 0;
    virtual void releaseInterface(unsigned number) = 0;
    // The device leaves the bus and comes back, as if unplugged: the way out of a phone's accessory mode.
    virtual bool reset() = 0;
    // The transfer and its data stay untouched until reap() gives it back.
    virtual bool submit(UsbTransfer* transfer) = 0;
    virtual UsbTransfer* reap() = 0;  // a finished transfer, or null
    virtual void cancel(UsbTransfer* transfer) = 0;
    virtual bool gone() const = 0;  // unplugged: nothing more will be reaped
    // What poll() must watch for reap() to have something.
    virtual int waitFd() const = 0;
    virtual short waitEvents() const = 0;
};

// The device with this bus and address, as /sys/bus/usb/devices gives them. Null with errno set.
UsbDevice* openUsbDevice(unsigned bus, unsigned address);

}  // namespace evo

#endif
