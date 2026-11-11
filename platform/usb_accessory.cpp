// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android phones on the USB ports and their switch to accessory mode (see usb_accessory.hpp).
#include "usb_accessory.hpp"

#include <dirent.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace evo {
namespace {

const unsigned kGoogleVendor = 0x18d1;
const unsigned kAccessoryProduct = 0x2d00;     // accessory
const unsigned kAccessoryAdbProduct = 0x2d01;  // accessory and the developer's debugging interface
const unsigned kAppleVendor = 0x05ac;
const unsigned kClassImage = 0x06;  // MTP and PTP
const unsigned kClassHub = 0x09;
const unsigned kClassVendor = 0xff;  // MTP on older phones, debugging, the accessory itself
const unsigned kBulk = 2;

// AOA requests, on endpoint 0 (vendor requests to the device).
const uint8_t kGetProtocol = 51;
const uint8_t kSendString = 52;
const uint8_t kStart = 53;
const unsigned kRequestMs = 500;
// What makes the phone open Android Auto (manufacturer and model), then what it shows about the accessory.
const char* const kIdentity[] = {
    "Android", "Android Auto", "Android Auto", "2.0.1", "https://github.com/Jiraiya43/entry-aad", "entry-aad",
};

// A small sysfs attribute as a number; false when missing or not one.
bool readNumber(const std::string& path, int base, unsigned& value) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        return false;
    }
    char text[32] = "";
    const bool got = fgets(text, sizeof(text), f) != nullptr;
    fclose(f);
    if (!got) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long n = strtoul(text, &end, base);
    if (errno || end == text || (*end && *end != '\n') || n > 0xffff) {
        return false;
    }
    value = static_cast<unsigned>(n);
    return true;
}

bool isAccessory(unsigned vendor, unsigned product) {
    return vendor == kGoogleVendor && (product == kAccessoryProduct || product == kAccessoryAdbProduct);
}

// The entries of the folder whose name starts with `prefix`.
std::vector<std::string> entries(const std::string& folder, const std::string& prefix) {
    std::vector<std::string> names;
    DIR* dir = opendir(folder.c_str());
    if (!dir) {
        return names;
    }
    while (dirent* entry = readdir(dir)) {
        if (!strncmp(entry->d_name, prefix.c_str(), prefix.size())) {
            names.push_back(entry->d_name);
        }
    }
    closedir(dir);
    return names;
}

// The kernel knows the device is soldered in ("fixed"): never a phone.
bool fixedDevice(const std::string& device) {
    FILE* f = fopen((device + "/removable").c_str(), "r");
    if (!f) {
        return false;
    }
    char text[16] = "";
    const bool got = fgets(text, sizeof(text), f) != nullptr;
    fclose(f);
    return got && !strncmp(text, "fixed", 5);
}

bool looksLikeAndroid(const std::string& sysfs, const std::string& port) {
    const std::vector<std::string> interfaces = entries(sysfs, port + ":");
    for (size_t i = 0; i < interfaces.size(); ++i) {
        unsigned kind = 0;
        if (readNumber(sysfs + "/" + interfaces[i] + "/bInterfaceClass", 16, kind) &&
            (kind == kClassImage || kind == kClassVendor)) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool validUsbPort(const std::string& port) {
    // digits '-' digits ('.' digits)*
    size_t i = 0;
    const size_t n = port.size();
    if (n > 32) {
        return false;
    }
    for (unsigned part = 0;; ++part) {
        const size_t start = i;
        while (i < n && port[i] >= '0' && port[i] <= '9') {
            ++i;
        }
        if (i == start) {
            return false;
        }
        if (i == n) {
            return part >= 1;
        }
        if (port[i] != (part == 0 ? '-' : '.')) {
            return false;
        }
        ++i;
    }
}

bool readUsbDevice(const char* sysfs, const std::string& port, UsbPhone& out) {
    const std::string device = std::string(sysfs) + "/" + port;
    UsbPhone phone;
    phone.port = port;
    if (!validUsbPort(port) || !readNumber(device + "/busnum", 10, phone.bus) ||
        !readNumber(device + "/devnum", 10, phone.address) || !readNumber(device + "/idVendor", 16, phone.vendor) ||
        !readNumber(device + "/idProduct", 16, phone.product)) {
        return false;
    }
    phone.accessory = isAccessory(phone.vendor, phone.product);
    out = phone;
    return true;
}

void listUsbPhones(const char* sysfs, const std::string& onlyPort, std::vector<UsbPhone>& out) {
    out.clear();
    std::vector<UsbPhone> others;
    const std::vector<std::string> names = entries(sysfs, onlyPort);
    for (size_t i = 0; i < names.size(); ++i) {
        // Devices only: interfaces have a ':' and root hubs are named "usbN", neither is a valid port name.
        UsbPhone phone;
        unsigned kind = 0;
        if ((!onlyPort.empty() && names[i] != onlyPort) || !readUsbDevice(sysfs, names[i], phone) ||
            !readNumber(std::string(sysfs) + "/" + names[i] + "/bDeviceClass", 16, kind) || kind == kClassHub) {
            continue;
        }
        if (phone.accessory) {
            out.push_back(phone);
        } else if (phone.vendor != kAppleVendor && !fixedDevice(std::string(sysfs) + "/" + names[i]) &&
                   looksLikeAndroid(sysfs, names[i])) {
            others.push_back(phone);
        }
    }
    out.insert(out.end(), others.begin(), others.end());
}

bool readAccessoryEndpoints(const char* sysfs, const std::string& port, AccessoryEndpoints& out) {
    if (!validUsbPort(port)) {
        return false;
    }
    // Configuration 1, interface 0: where AOA puts the accessory, before the debugging interface when there is one.
    const std::string interface = std::string(sysfs) + "/" + port + ":1.0";
    unsigned kind = 0, subclass = 0;
    if (!readNumber(interface + "/bInterfaceClass", 16, kind) || kind != kClassVendor ||
        !readNumber(interface + "/bInterfaceSubClass", 16, subclass) || subclass != 0xff) {
        return false;
    }
    AccessoryEndpoints found;
    const std::vector<std::string> names = entries(interface, "ep_");
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string endpoint = interface + "/" + names[i];
        unsigned address = 0, attributes = 0, packet = 0;
        if (!readNumber(endpoint + "/bEndpointAddress", 16, address) ||
            !readNumber(endpoint + "/bmAttributes", 16, attributes) ||
            !readNumber(endpoint + "/wMaxPacketSize", 16, packet) || (attributes & 3) != kBulk || address > 0xff) {
            continue;
        }
        if ((address & 0x80) && !found.in) {
            found.in = static_cast<uint8_t>(address);
        } else if (!(address & 0x80) && !found.out) {
            found.out = static_cast<uint8_t>(address);
            found.outPacket = packet & 0x7ff;
        }
    }
    if (!found.in || !found.out || !found.outPacket) {
        return false;
    }
    out = found;
    return true;
}

int readUsbSetting(const char* path, std::string& port) {
    port.clear();
    FILE* f = fopen(path, "r");
    if (!f) {
        return errno == ENOENT ? 0 : -1;
    }
    char text[64] = "";
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    const bool whole = feof(f) != 0;
    fclose(f);
    std::string value(text, n);
    while (!value.empty() &&
           (value[value.size() - 1] == '\n' || value[value.size() - 1] == '\r' || value[value.size() - 1] == ' ')) {
        value.erase(value.size() - 1);
    }
    if (!whole || (!value.empty() && !validUsbPort(value))) {
        return -1;
    }
    port = value;
    return 1;
}

AccessorySwitch askAccessoryMode(UsbDevice& device, unsigned& protocol) {
    uint8_t version[2] = {0, 0};
    const int n = device.control(0xc0, kGetProtocol, 0, 0, version, sizeof(version), kRequestMs);
    protocol = version[0] | (static_cast<unsigned>(version[1]) << 8);
    if (n != static_cast<int>(sizeof(version)) || protocol < 1) {
        return kSwitchNotAndroid;
    }
    for (unsigned i = 0; i < sizeof(kIdentity) / sizeof(kIdentity[0]); ++i) {
        // The string with its ending zero, as the protocol asks.
        std::vector<uint8_t> text(kIdentity[i], kIdentity[i] + strlen(kIdentity[i]) + 1);
        if (device.control(0x40, kSendString, 0, static_cast<uint16_t>(i), &text[0], static_cast<uint16_t>(text.size()),
                           kRequestMs) != static_cast<int>(text.size())) {
            return kSwitchFailed;
        }
    }
    // The phone may leave the bus before it acknowledges: only its return as an accessory tells that it worked.
    device.control(0x40, kStart, 0, 0, nullptr, 0, kRequestMs);
    return kSwitchAsked;
}

}  // namespace evo
