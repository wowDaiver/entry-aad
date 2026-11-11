// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto over a USB cable: carries the bytes between a phone in accessory mode and one end of a local socket.
// The other end goes to the session exactly as a phone's TCP connection does, so the session knows nothing of USB
// (the bridge is the idea of Sq00ky's NBT-AAD; this one runs on the kernel's usbfs).
#ifndef ENTRY_AAD_PLATFORM_USB_BRIDGE_HPP
#define ENTRY_AAD_PLATFORM_USB_BRIDGE_HPP

#include <pthread.h>
#include <stdint.h>

#include <atomic>

#include "usb_accessory.hpp"
#include "usb_device.hpp"

namespace evo {

class UsbBridge {
public:
    // Takes both: the phone, its interface 0 claimed, and the bridge's end of the socket pair. When the bridge ends
    // (the phone is unplugged or stops answering, or the session closed its end) the interface is released and both
    // are closed, which the session sees as the phone leaving.
    UsbBridge(UsbDevice* device, const AccessoryEndpoints& endpoints, int socketFd);
    ~UsbBridge();  // ends the bridge and joins its thread

    bool start();
    bool finished() const { return finished_; }
    // Once finished.
    bool phoneLeft() const { return phoneLeft_; }  // the USB side ended it, not the session
    uint64_t bytesFromPhone() const { return fromPhone_; }
    uint64_t bytesToPhone() const { return toPhone_; }

private:
    static void* threadMain(void* self);
    void run();
    const char* carry();  // why it ended

    UsbDevice* device_;
    AccessoryEndpoints endpoints_;
    int socket_;
    int wake_[2];  // written by the destructor: the thread leaves poll()
    pthread_t thread_;
    bool started_;
    std::atomic<bool> stop_, finished_, phoneLeft_;
    std::atomic<uint64_t> fromPhone_, toPhone_;
};

}  // namespace evo

#endif
