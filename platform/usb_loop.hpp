// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The USB half of a wired start: watches the USB ports for an Android phone, switches it to accessory mode and hands
// the service a socket to it, one phone at a time. The wired counterpart of wireless_loop.
#ifndef ENTRY_AAD_PLATFORM_USB_LOOP_HPP
#define ENTRY_AAD_PLATFORM_USB_LOOP_HPP

#include <string>

#include "usb_device.hpp"

namespace evo {

// What the loop asks of the service, from the loop's own thread.
class UsbHost {
public:
    virtual bool stopRequested() = 0;
    // A phone connection is being prepared or its Android Auto session runs: no new phone meanwhile.
    virtual bool projectionBusy() = 0;
    // A phone is ready: the socket carries its Android Auto bytes, as a TCP connection does. The host owns it.
    virtual void usbPhoneReady(int fd) = 0;

protected:
    ~UsbHost() {}
};

struct UsbLoopSetup {
    std::string port;                                    // empty: every port; else the only one looked at
    const char* sysfs;                                   // where the kernel lists the devices
    UsbDevice* (*open)(unsigned bus, unsigned address);  // openUsbDevice; the tests have their own
    unsigned turnMs;                                     // between two looks at the bus
    unsigned afterResetMs;                               // a reset phone leaves the bus and comes back: not looked at
    UsbLoopSetup() : sysfs("/sys/bus/usb/devices"), open(openUsbDevice), turnMs(500), afterResetMs(2000) {}
};

// Runs until a stop is requested.
//
// A phone in its normal mode is asked to become an accessory (askAccessoryMode); it comes back on the same port and
// its interface 0 is claimed at once, before the car's own USB service looks at it. An interface another program
// holds is never taken from it. An accessory this loop cannot use (left by an earlier session, made by the car for
// itself, or held by another program) is no use to the phone either: the phone alone is reset, which is what
// unplugging it does, and it is
// asked again. A phone that says nothing may be showing a question to its owner: it is tried again as it is, without
// a reset. After 3 resets with no session that held, the phone is left alone until it is plugged in again.
void runUsbLoop(const UsbLoopSetup& setup, UsbHost& host);

}  // namespace evo

#endif
