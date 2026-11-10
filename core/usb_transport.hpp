// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// AOA connection restricted to an explicit sysfs port and the phone's pre-accessory VID/PID.
#ifndef ENTRY_AAD_USB_TRANSPORT_HPP
#define ENTRY_AAD_USB_TRANSPORT_HPP

#include <signal.h>

#include <string>

#include "aa_transport.hpp"

namespace aa {
struct UsbIdentity {
    unsigned bus, address, vendor, product, deviceClass;
    bool accessory() const;
};
struct UsbSelection {
    std::string port;
    unsigned vendor, product;
    bool adoptAccessory;  // explicit CLI opt-in for a phone already in AOA mode
    bool automatic;       // optional "auto " prefix in the selection file; legacy triples remain USB-only
    UsbSelection() : vendor(0), product(0), adoptAccessory(false), automatic(false) {}
    bool valid() const;
    // 0 absent (wireless), 1 valid opt-in, -1 unsafe/malformed. Format: [auto ]PORT HEX_VID HEX_PID.
    int loadFile(const char* path);
};
bool readUsbIdentity(const UsbSelection& selection, UsbIdentity& id, const char* sysfs = "/sys/bus/usb/devices");

// Narrow injectable boundary. Production uses the installed libusb-1.0.so.0 dynamically.
// No detach/reset/set-configuration/clear-halt API exists at this boundary.
class UsbBackend {
public:
    virtual ~UsbBackend() {}
    virtual bool open(unsigned bus, unsigned address) = 0;
    virtual void close() = 0;
    // Production reads libusb's cached device descriptor, avoiding a control round trip before a fresh AOA claim.
    virtual int deviceDescriptor(uint8_t* data) { return control(0x80, 6, 0x0100, 0, data, 18, 250); }
    virtual int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t size,
                        unsigned timeoutMs) = 0;
    virtual int claim(unsigned interface) = 0;
    virtual int release(unsigned interface) = 0;
    virtual int bulk(uint8_t endpoint, uint8_t* data, int size, int& transferred, unsigned timeoutMs) = 0;
};
UsbBackend* libusbBackend();

// Takes backend ownership on every path, even invalid selection, cancellation and failed negotiation.
// Returns an owned byte stream. Waiting for re-enumeration checks quit every 50 ms (15 s maximum).
Transport* connectUsb(const UsbSelection& selection, const volatile sig_atomic_t* quit, UsbBackend* backend,
                      const char* sysfs = "/sys/bus/usb/devices");

// Bound retries for one attachment/session failure. Requires a changed device address or process restart after 3.
class UsbRetryBudget {
public:
    UsbRetryBudget() : bus_(0), address_(0), attempts_(0) {}
    bool take(const UsbIdentity& id);
    void followReenumeration(const UsbIdentity& id);

private:
    unsigned bus_, address_, attempts_;
};
}  // namespace aa
#endif
