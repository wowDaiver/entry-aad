// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// USB identity, negotiation, endpoint, short transfer and cleanup failures on a fake bus.
#include <memory>

#include "fake_usb_backend.hpp"

int main() {
    FakeUsbTree tree;
    aa::UsbSelection selection = tree.selection();
    const char* badPorts[] = {"", "usb3", "3", "3-0", "3-1/../2", "3-1:1.0", "3-1.", "3--1", "256-1", "3-1..2"};
    for (unsigned i = 0; i < sizeof(badPorts) / sizeof(badPorts[0]); ++i) {
        selection.port = badPorts[i];
        assert(!selection.valid());
    }
    selection = tree.selection();
    assert(selection.valid());
    aa::UsbIdentity id;
    assert(aa::readUsbIdentity(selection, id, tree.root.c_str()));
    assert(id.vendor == 0x04e8 && id.address == 2 && !id.accessory());
    aa::UsbRetryBudget budget;
    assert(budget.take(id) && budget.take(id) && budget.take(id) && !budget.take(id));
    ++id.address;
    budget.followReenumeration(id);
    assert(!budget.take(id));
    ++id.address;
    assert(budget.take(id));

    const std::string file = tree.root + "/selection";
    aa::UsbSelection loaded;
    assert(loaded.loadFile(file.c_str()) == 0);
    FILE* config = fopen(file.c_str(), "w");
    assert(config);
    fputs("3-1 04e8 6860\n", config);
    fclose(config);
    assert(loaded.loadFile(file.c_str()) == 1 && loaded.port == "3-1" && loaded.vendor == 0x04e8 && !loaded.automatic);
    config = fopen(file.c_str(), "w");
    assert(config);
    fputs("auto 3-1 04e8 6860\n", config);
    fclose(config);
    assert(loaded.loadFile(file.c_str()) == 1 && loaded.automatic && loaded.port == "3-1");
    assert(chmod(file.c_str(), 0666) == 0);
    assert(loaded.loadFile(file.c_str()) < 0);
    assert(chmod(file.c_str(), 0600) == 0);
    config = fopen(file.c_str(), "w");
    assert(config);
    fputs("3-1 04e8 6860 extra\n", config);
    fclose(config);
    assert(loaded.loadFile(file.c_str()) < 0);
    assert(unlink(file.c_str()) == 0);
    assert(symlink("/dev/null", file.c_str()) == 0);
    assert(loaded.loadFile(file.c_str()) < 0);
    assert(unlink(file.c_str()) == 0);

    // Before AOA: deny other phone, hub, invalid config selector and already-accessory without explicit opt-in.
    for (int mode = 0; mode < 4; ++mode) {
        tree.identity(false);
        FakeUsbStats stats;
        aa::UsbSelection s = tree.selection();
        if (mode == 0) {
            s.product = 0x1234;
        }
        if (mode == 1) {
            tree.field("bDeviceClass", "09");
        }
        if (mode == 2) {
            s.port = "3-1/../../";
        }
        if (mode == 3) {
            tree.identity(true);
        }
        assert(!aa::connectUsb(s, nullptr, new FakeUsbBackend(tree, stats), tree.root.c_str()));
        assert(!stats.opens && !stats.starts && !stats.claims);
    }
    // Negotiation failures must not claim anything; cancellation works before/after control requests.
    for (int mode = 0; mode < 8; ++mode) {
        tree.identity(false);
        FakeUsbStats stats;
        FakeUsbBackend* backend = new FakeUsbBackend(tree, stats);
        volatile sig_atomic_t quit = 0;
        if (mode == 0) {
            backend->mismatch = true;
        }
        if (mode == 1) {
            backend->protocolSize = 1;
        }
        if (mode == 2) {
            backend->protocolVersion = 3;
        }
        if (mode == 3) {
            backend->failString = 2;
        }
        if (mode == 4) {
            backend->startRc = -1;
        }
        if (mode == 5) {
            quit = 1;
        }
        if (mode == 6 || mode == 7) {
            backend->quit = &quit;
            backend->cancelRequest = mode == 6 ? 51 : 53;
            backend->reenumerate = false;
        }
        assert(!aa::connectUsb(tree.selection(), &quit, backend, tree.root.c_str()));
        assert(stats.opens == stats.closes && !stats.claims && !stats.releases);
    }
    // Malformed/ambiguous endpoints and wrong active configuration are never repaired by resetting the device.
    for (int mode = 0; mode < 7; ++mode) {
        tree.identity(true);
        FakeUsbStats stats;
        FakeUsbBackend* backend = new FakeUsbBackend(tree, stats);
        if (mode == 0) {
            backend->config[18] = 0;  // descriptor bLength
        }
        if (mode == 1) {
            backend->config[27] = 0x82;  // two IN endpoints
        }
        if (mode == 2) {
            backend->config[21] = 3;  // interrupt instead of bulk
        }
        if (mode == 3) {
            backend->config[11] = 1;  // only ADB interface left
        }
        if (mode == 4) {
            backend->activeConfig = 0;
        }
        if (mode == 5) {
            backend->config[2] = 54;  // truncation
        }
        if (mode == 6) {
            backend->claimRc = -6;  // occupied by another driver/userspace owner
        }
        assert(!aa::connectUsb(tree.selection(true), nullptr, backend, tree.root.c_str()));
        assert(!stats.releases && stats.opens == stats.closes);
        assert(stats.claims == (mode == 6 ? 1u : 0u));
    }
    // Genuine AOA re-enumeration, including START disappearing before its control completion.
    for (int startRc = 0; startRc >= -4; startRc -= 4) {
        tree.identity(false);
        FakeUsbStats stats;
        FakeUsbBackend* backend = new FakeUsbBackend(tree, stats);
        backend->startRc = startRc;
        std::unique_ptr<aa::Transport> stream(aa::connectUsb(tree.selection(), nullptr, backend, tree.root.c_str()));
        assert(stream && stats.opens == 2 && stats.strings == 6 && stats.starts == 1 && stats.claims == 1);
        backend->reads.push_back(FakeBulk(-7, 1000));  // timeout with received bytes
        uint8_t bytes[1000];
        bool woke = true;
        assert(stream->read(bytes, 4, -1, woke) == 4 && !woke);
        assert(stream->read(bytes + 4, 996, -1, woke) == 996 && stats.bulkReads == 1);
        for (unsigned i = 0; i < sizeof(bytes); ++i) {
            assert(bytes[i] == 0x5a);
        }
        backend->reads.push_back(FakeBulk(-7, 0));
        assert(stream->read(bytes, 1, -1, woke) == 0);
        backend->writes.push_back(FakeBulk(-7, 3));
        backend->writes.push_back(FakeBulk(0, 5));
        const uint8_t message[] = {0, 1, 2, 3, 4, 5, 6, 7};
        assert(stream->write(message, 8) == 3 && stream->write(message + 3, 5) == 5);
        assert(backend->sent == std::vector<uint8_t>(message, message + 8));
        std::vector<uint8_t> aligned(512, 0x33);
        backend->writes.push_back(FakeBulk(0, 512));
        backend->writes.push_back(FakeBulk(0, 0));
        assert(stream->write(&aligned[0], aligned.size()) == 512 && stream->endWrite());
        assert(backend->writes.empty());  // exact max-packet multiple got one ZLP, not a duplicate payload
        const unsigned writesBefore = stats.bulkWrites;
        assert(stream->endWrite() && stats.bulkWrites == writesBefore);
        int wake[2];
        assert(pipe(wake) == 0);
        assert(::write(wake[1], "x", 1) == 1);
        assert(stream->read(bytes, 1, wake[0], woke) == 0 && woke);
        backend->reads.push_back(FakeBulk(0, 2));
        const unsigned readsBefore = stats.bulkReads;
        assert(stream->read(bytes, 1, wake[0], woke) == 1 && woke);
        assert(stream->read(bytes, 1, wake[0], woke) == 1 && woke && stats.bulkReads == readsBefore + 1);

        close(wake[0]);
        close(wake[1]);
        backend->reads.push_back(FakeBulk(-4, 0));
        assert(stream->read(bytes, 1, -1, woke) < 0);  // unplug
        stream->cancel();
        assert(stream->write(message, 8) < 0 && stream->read(bytes, 1, -1, woke) < 0);
        stream.reset();
        assert(stats.releases == 1 && stats.closes == 2);
    }
    // Reproduce the car race: BMW grabs the new accessory if descriptor I/O precedes our claim.
    for (int mode = 0; mode < 3; ++mode) {
        tree.identity(false);
        FakeUsbStats stats;
        FakeUsbBackend* backend = new FakeUsbBackend(tree, stats);
        backend->seizeBeforeConfig = true;
        if (mode == 1) {
            backend->config[18] = 0;  // fail after reservation: it must still release
        }
        if (mode == 2) {
            backend->claimRc = -6;  // already owned: never steal it
        }
        aa::Transport* stream = aa::connectUsb(tree.selection(), nullptr, backend, tree.root.c_str());
        assert((stream != nullptr) == (mode == 0));
        delete stream;
        assert(stats.claims == 1 && stats.releases == (mode == 2 ? 0u : 1u));
        assert(stats.opens == stats.closes);
    }
    // Open/claim/release repeat across unplug/replug; no stale handle, endpoint or buffer remains.
    for (int n = 0; n < 20; ++n) {
        tree.identity(false);
        FakeUsbStats stats;
        aa::Transport* stream =
            aa::connectUsb(tree.selection(), nullptr, new FakeUsbBackend(tree, stats), tree.root.c_str());
        assert(stream);
        delete stream;
        assert(stats.claims == 1 && stats.releases == 1 && stats.opens == stats.closes);
    }
    puts("PASS USB fake-bus: selection, AOA, descriptors, partial I/O, wake, unplug/replug, cleanup");
}
