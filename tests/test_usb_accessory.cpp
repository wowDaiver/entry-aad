// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android phones on a fake USB bus: which devices are asked, the accessory's endpoints, the switch requests, the
// setting file.
#include "usb_accessory.hpp"
#include "usb_fakes.hpp"

namespace {

void portNames() {
    const char* const good[] = {"1-1", "3-1.2", "12-3.4.5"};
    const char* const bad[] = {"", "1", "usb1", "1-", "-1", "1-1.", "1-1:1.0", "1-1/..", "1--1", "1-1 ", "a-1", "1.1"};
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        CHECK(evo::validUsbPort(good[i]));
    }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(!evo::validUsbPort(bad[i]));
    }
}

void whoIsAsked() {
    fake::Bus bus;
    bus.device("1-1", 1, 2, 0x0781, 0x5567, 0, 8);     // USB stick
    bus.device("1-2", 1, 3, 0x05ac, 0x12a8, 0, 6);     // iPhone: the car's own business
    bus.device("1-3", 1, 4, 0x0424, 0x2514, 9, 9);     // hub
    bus.device("1-3.1", 1, 5, 0x04e8, 0x6860, 0, 6);   // Android phone behind it, file transfer
    bus.device("2-1", 2, 2, 0x22b8, 0x2e76, 0, 0xff);  // Android phone showing a vendor interface only
    bus.device("2-2", 2, 3, 0x0a12, 0x0001, 0, 0xff);  // a module soldered in the unit
    bus.put("2-2", "removable", "fixed");
    bus.device("2-3", 2, 4, 0x046d, 0xc31c, 0, 3);  // keyboard
    bus.accessory("3-1", 3, 7);
    bus.put("usb1", "busnum", "1");  // root hub: not a port name
    std::vector<evo::UsbPhone> phones;
    evo::listUsbPhones(bus.root.c_str(), "", phones);
    CHECK(phones.size() == 3);
    CHECK(phones[0].port == "3-1" && phones[0].accessory && phones[0].bus == 3 && phones[0].address == 7);
    std::set<std::string> others;
    others.insert(phones[1].port);
    others.insert(phones[2].port);
    CHECK(others.count("1-3.1") && others.count("2-1") && !phones[1].accessory && !phones[2].accessory);
    // One port only.
    evo::listUsbPhones(bus.root.c_str(), "1-3.1", phones);
    CHECK(phones.size() == 1 && phones[0].vendor == 0x04e8 && phones[0].product == 0x6860 && phones[0].address == 5);
    evo::listUsbPhones(bus.root.c_str(), "1-3", phones);  // the hub itself, not what is behind it
    CHECK(phones.empty());
    evo::listUsbPhones(bus.root.c_str(), "4-1", phones);
    CHECK(phones.empty());
    // An unreadable entry is no device.
    bus.put("2-1", "devnum", "soon");
    evo::listUsbPhones(bus.root.c_str(), "2-1", phones);
    CHECK(phones.empty());
    evo::UsbPhone one;
    CHECK(evo::readUsbDevice(bus.root.c_str(), "1-1", one) && one.vendor == 0x0781 && !one.accessory);
    CHECK(!evo::readUsbDevice(bus.root.c_str(), "9-9", one) && !evo::readUsbDevice(bus.root.c_str(), "../x", one));
}

void endpoints() {
    fake::Bus bus;
    bus.accessory("1-1", 1, 3);
    evo::AccessoryEndpoints found;
    CHECK(evo::readAccessoryEndpoints(bus.root.c_str(), "1-1", found));
    CHECK(found.in == 0x81 && found.out == 0x01 && found.outPacket == 512);
    // Not yet set up by the kernel, not the accessory interface, an endpoint that is not bulk.
    CHECK(!evo::readAccessoryEndpoints(bus.root.c_str(), "1-2", found));
    bus.device("1-2", 1, 4, 0x18d1, 0x2d01, 0, 6);
    CHECK(!evo::readAccessoryEndpoints(bus.root.c_str(), "1-2", found));
    bus.put("1-1:1.0/ep_01", "bmAttributes", "03");
    CHECK(!evo::readAccessoryEndpoints(bus.root.c_str(), "1-1", found));
}

void switchRequests() {
    fake::Phone phone;
    {
        fake::Device device(phone);
        unsigned protocol = 0;
        CHECK(evo::askAccessoryMode(device, protocol) == evo::kSwitchAsked && protocol == 2);
    }
    CHECK(phone.strings.size() == 6 && phone.strings[0] == "Android" && phone.strings[1] == "Android Auto");
    CHECK(phone.started == 1);
    fake::Phone other;
    other.android = false;
    {
        fake::Device device(other);
        unsigned protocol = 0;
        CHECK(evo::askAccessoryMode(device, protocol) == evo::kSwitchNotAndroid);
    }
    CHECK(other.protocolAsked == 1 && other.strings.empty() && other.started == 0);  // nothing more was sent to it
}

void settingFile() {
    fake::Bus folder;
    const std::string path = folder.root + "/usb";
    std::string port = "x";
    CHECK(evo::readUsbSetting(path.c_str(), port) == 0 && port.empty());
    folder.put("", "usb", "");  // an empty line
    CHECK(evo::readUsbSetting(path.c_str(), port) == 1 && port.empty());
    FILE* f = fopen(path.c_str(), "w");
    CHECK(f != nullptr);
    fclose(f);  // as `touch` leaves it
    CHECK(evo::readUsbSetting(path.c_str(), port) == 1 && port.empty());
    folder.put("", "usb", "3-1.2");
    CHECK(evo::readUsbSetting(path.c_str(), port) == 1 && port == "3-1.2");
    folder.put("", "usb", "auto 3-1 04e8 6860");
    CHECK(evo::readUsbSetting(path.c_str(), port) == -1);
    folder.put("", "usb", std::string(200, '1'));
    CHECK(evo::readUsbSetting(path.c_str(), port) == -1);
}

}  // namespace

int main() {
    portNames();
    whoIsAsked();
    endpoints();
    switchRequests();
    settingFile();
    puts("PASS USB accessory: port names, devices asked and left alone, endpoints, switch requests, setting file");
    return 0;
}
