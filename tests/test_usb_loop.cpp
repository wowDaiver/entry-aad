// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The USB loop on a fake bus: a phone is switched and handed over, a stale accessory is reset first, other devices
// are left alone, a held interface is never taken, and resets are counted.
#include <sys/socket.h>

#include <atomic>

#include "usb_accessory.hpp"
#include "usb_fakes.hpp"
#include "usb_loop.hpp"

namespace {

std::vector<fake::Phone*> g_phones;
std::atomic<unsigned> g_refused(0);  // opens of a device that is no phone of the test

evo::UsbDevice* openFake(unsigned bus, unsigned address) {
    for (size_t i = 0; i < g_phones.size(); ++i) {
        if (g_phones[i]->busNumber == bus && g_phones[i]->address == address) {
            return new fake::Device(*g_phones[i]);
        }
    }
    ++g_refused;
    errno = ENOENT;
    return nullptr;
}

struct Host : evo::UsbHost {
    std::atomic<bool> stop, busy;
    std::atomic<int> fd;
    std::atomic<unsigned> handed;
    Host() : stop(false), busy(false), fd(-1), handed(0) {}
    bool stopRequested() override { return stop; }
    bool projectionBusy() override { return busy; }
    void usbPhoneReady(int socket) override {
        fd = socket;
        busy = true;
        ++handed;
    }
    // The session ends: it closes its socket.
    void endSession() {
        close(fd);
        fd = -1;
        busy = false;
    }
};

struct Run {
    Host host;
    evo::UsbLoopSetup setup;
    pthread_t thread;
    explicit Run(const fake::Bus& bus, const char* port = "") {
        setup.sysfs = bus.root.c_str();
        setup.port = port;
        setup.open = openFake;
        setup.turnMs = 50;
        setup.afterResetMs = 100;
        g_refused = 0;
        CHECK(pthread_create(&thread, nullptr, main, this) == 0);
    }
    static void* main(void* self) {
        Run* run = static_cast<Run*>(self);
        evo::runUsbLoop(run->setup, run->host);
        return nullptr;
    }
    void end() {
        host.stop = true;
        pthread_join(thread, nullptr);
        if (host.fd >= 0) {
            close(host.fd);
        }
    }
    bool handed(unsigned count) {
        return fake::within(4000, [&]() { return host.handed == count; });
    }
};

void phonePluggedIn() {
    fake::Bus bus;
    fake::Phone phone;
    g_phones.assign(1, &phone);
    bus.device("1-1", 1, 9, 0x0781, 0x5567, 0, 8);  // a USB stick next to it
    phone.plug(bus, "1-2");
    Run run(bus);
    CHECK(run.handed(1));
    CHECK(phone.count(&fake::Phone::started) == 1 && phone.count(&fake::Phone::claims) == 1 &&
          phone.count(&fake::Phone::resets) == 0 && g_refused == 0);
    // The socket is the phone: bytes both ways.
    const uint8_t frame[] = {0, 3, 0, 2, 0xab, 0xcd};
    CHECK(send(run.host.fd, frame, sizeof(frame), 0) == sizeof(frame));
    CHECK(fake::within(2000, [&]() { return phone.received().size() == 1; }));
    phone.say(std::vector<uint8_t>(5, 0x42));
    uint8_t got[5];
    CHECK(recv(run.host.fd, got, sizeof(got), MSG_WAITALL) == 5 && got[4] == 0x42);
    // The session ends while the phone stays plugged in: its accessory mode is stale, so it is reset and asked again.
    run.host.endSession();
    CHECK(run.handed(2));
    CHECK(phone.count(&fake::Phone::resets) == 1 && phone.count(&fake::Phone::started) == 2);
    // The service stops: the phone is taken out of accessory mode.
    run.end();
    CHECK(phone.count(&fake::Phone::resets) == 2 && phone.count(&fake::Phone::releases) == 2);
    evo::UsbPhone now;
    CHECK(evo::readUsbDevice(bus.root.c_str(), "1-2", now) && !now.accessory);
}

// Left in accessory mode by an earlier service, or switched by the car: nobody listens on the phone.
void staleAccessory() {
    fake::Bus bus;
    fake::Phone phone;
    g_phones.assign(1, &phone);
    phone.plug(bus, "3-1", true);
    Run run(bus);
    CHECK(run.handed(1));
    CHECK(phone.count(&fake::Phone::resets) == 1 && phone.count(&fake::Phone::started) == 1);
    run.end();
}

void otherDevices() {
    fake::Bus bus;
    fake::Phone gadget;  // a vendor interface, but no Android
    gadget.android = false;
    gadget.vendor = 0x1234;
    g_phones.assign(1, &gadget);
    gadget.plug(bus, "1-1");
    bus.device("1-2", 1, 20, 0x05ac, 0x12a8, 0, 6);  // iPhone
    bus.device("1-3", 1, 21, 0x0424, 0x2514, 9, 9);  // hub
    bus.device("1-4", 1, 22, 0x0781, 0x5567, 0, 8);  // USB stick
    Run run(bus);
    usleep(600000);
    run.end();
    // Asked once whether it is an Android phone, then never again; the others were never opened.
    CHECK(gadget.protocolAsked == 1 && gadget.opens == 1 && gadget.started == 0 && gadget.resets == 0);
    CHECK(g_refused == 0 && run.host.handed == 0);
}

void onePortOnly() {
    fake::Bus bus;
    fake::Phone phone;
    g_phones.assign(1, &phone);
    phone.plug(bus, "1-2");
    Run run(bus, "1-1");
    usleep(400000);
    run.end();
    CHECK(phone.opens == 0 && run.host.handed == 0);
}

// Another program holds the accessory: it is never taken from it, and after 3 resets the phone is left alone.
void heldInterface() {
    fake::Bus bus;
    fake::Phone phone;
    phone.held = true;
    g_phones.assign(1, &phone);
    phone.plug(bus, "1-1");
    Run run(bus);
    CHECK(fake::within(15000, [&]() { return phone.count(&fake::Phone::resets) == 3; }));
    const unsigned started = phone.count(&fake::Phone::started);
    usleep(1500000);
    CHECK(phone.count(&fake::Phone::resets) == 3 && phone.count(&fake::Phone::started) <= started + 1);
    CHECK(run.host.handed == 0 && phone.count(&fake::Phone::releases) == 0);
    run.end();
}

// A session is running (wired or not): the bus is not touched.
void busyService() {
    fake::Bus bus;
    fake::Phone phone;
    g_phones.assign(1, &phone);
    phone.plug(bus, "1-1");
    Run* run = new Run(bus);
    CHECK(run->handed(1));
    fake::Phone second;
    second.address = 40;
    g_phones.push_back(&second);
    second.plug(bus, "1-2");
    usleep(400000);
    CHECK(second.opens == 0);
    run->end();
    delete run;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const int sink = open("/dev/null", O_WRONLY);  // the loop's own log lines
    const int console = dup(1);
    CHECK(sink >= 0 && console >= 0 && dup2(sink, 1) >= 0);
    phonePluggedIn();
    staleAccessory();
    otherDevices();
    onePortOnly();
    heldInterface();
    busyService();
    fflush(stdout);
    dup2(console, 1);
    puts(
        "PASS USB loop: switch and hand-over, stale accessory reset, other devices untouched, one port, held "
        "interface, busy service");
    return 0;
}
