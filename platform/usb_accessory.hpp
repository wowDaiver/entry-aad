// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Finding an Android phone on the car's USB ports and asking it to become an Android Open Accessory (AOA), the mode
// Android Auto speaks over a cable. Written from the public AOA protocol (source.android.com/docs/core/interaction/
// accessories/aoa). What is on the bus is read in /sys/bus/usb/devices: only a device that looks like an Android phone
// there is ever sent a request.
#ifndef ENTRY_AAD_PLATFORM_USB_ACCESSORY_HPP
#define ENTRY_AAD_PLATFORM_USB_ACCESSORY_HPP

#include <stdint.h>

#include <string>
#include <vector>

#include "usb_device.hpp"

namespace evo {

// A device that may be a phone, or is one in accessory mode.
struct UsbPhone {
    std::string port;  // its place on the bus as the kernel names it: "1-1", "3-1.2"
    unsigned bus, address, vendor, product;
    bool accessory;  // already in accessory mode (Google's 18d1:2d00 or 18d1:2d01)
    UsbPhone() : bus(0), address(0), vendor(0), product(0), accessory(false) {}
};

// The two bulk endpoints of the accessory interface (interface 0).
struct AccessoryEndpoints {
    uint8_t in, out;
    unsigned outPacket;  // the largest packet out: a transfer of a multiple of it needs an empty one after it
    AccessoryEndpoints() : in(0), out(0), outPacket(0) {}
};

// A port name as the kernel writes it: bus, dash, port, then ".port" per hub. Nothing else, so it can go in a path.
bool validUsbPort(const std::string& port);

// The devices worth asking, the accessories first. Left out: hubs, Apple devices (the car's CarPlay and iPod own
// them), devices the kernel knows to be soldered in, and every device that shows neither a picture transfer (MTP/PTP)
// nor a vendor interface, which is what an Android phone shows whatever its USB mode: so USB sticks, keyboards, audio
// and Bluetooth devices are never asked. onlyPort: empty, or the one port to look at.
void listUsbPhones(const char* sysfs, const std::string& onlyPort, std::vector<UsbPhone>& out);
// The device at that port right now, whatever it is; false when there is none or it cannot be read.
bool readUsbDevice(const char* sysfs, const std::string& port, UsbPhone& out);
// False until the kernel has set the accessory up, or when its interface 0 is not the accessory one.
bool readAccessoryEndpoints(const char* sysfs, const std::string& port, AccessoryEndpoints& out);

// The USB setting file (service_contract.hpp, kUsb): 0 when there is none (wireless), 1 when Android Auto goes over
// USB, with `port` empty for every port or holding the one named in the file, -1 when the file holds anything else.
int readUsbSetting(const char* path, std::string& port);

enum AccessorySwitch {
    kSwitchAsked,       // the phone knows AOA and was told to start: it leaves the bus and comes back as an accessory
    kSwitchNotAndroid,  // it does not answer the AOA version request: not asked again
    kSwitchFailed,      // it answered, then a request failed
};
AccessorySwitch askAccessoryMode(UsbDevice& device, unsigned& protocol);

}  // namespace evo

#endif
