// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Startup selection reads only the selected phone/port, distinguishes absence from unreadable USB state.
#include "fake_usb_backend.hpp"
#include "transport_policy.hpp"

int main() {
    FakeUsbTree tree;
    aa::UsbSelection selection = tree.selection();
    assert(evo::phonePresence(selection, tree.root.c_str()) == evo::kPhoneNormal);
    assert(evo::phonePresence(selection, "/missing-entry-aad-sysfs") == evo::kPresenceUnknown);
    tree.identity(true);
    assert(evo::phonePresence(selection, tree.root.c_str()) == evo::kPhoneAccessory);
    tree.field("idVendor", "1234");
    assert(evo::phonePresence(selection, tree.root.c_str()) == evo::kPhoneAbsent);
    tree.field("bDeviceClass", "09");
    assert(evo::phonePresence(selection, tree.root.c_str()) == evo::kPhoneAbsent);
    tree.identity(false);
    tree.field("devnum", "malformed");
    assert(evo::phonePresence(selection, tree.root.c_str()) == evo::kPresenceUnknown);
    tree.identity(false);
    const std::string port = tree.root + "/3-1", detached = tree.root + "/detached";
    assert(rename(port.c_str(), detached.c_str()) == 0);
    assert(evo::phonePresence(selection, tree.root.c_str()) == evo::kPhoneAbsent);
    assert(rename(detached.c_str(), port.c_str()) == 0);

    assert(evo::startupTransport(evo::kPhoneAbsent) == evo::kWirelessProjection);
    assert(evo::startupTransport(evo::kPhoneNormal) == evo::kUsbProjection);
    assert(evo::startupTransport(evo::kPhoneAccessory) == evo::kUsbProjection);
    puts("PASS startup selection: pinned phone/port, unrelated devices, unreadable state, USB and wireless");
}
