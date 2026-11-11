// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// For the USB tests: a folder shaped like /sys/bus/usb/devices and a phone behind the UsbDevice interface. No real
// device is opened.
#ifndef ENTRY_AAD_TESTS_USB_FAKES_HPP
#define ENTRY_AAD_TESTS_USB_FAKES_HPP

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <deque>
#include <set>
#include <string>
#include <vector>

#include "usb_device.hpp"

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                            \
        }                                                       \
    } while (0)

namespace fake {

inline int removeEntry(const char* path, const struct stat*, int, struct FTW*) {
    return remove(path);
}

inline uint32_t nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec) * 1000u + static_cast<uint32_t>(ts.tv_nsec / 1000000);
}

// The kernel's list of USB devices, in a temporary folder.
struct Bus {
    std::string root;

    Bus() {
        char path[] = "/tmp/entry-aad-usb-XXXXXX";
        CHECK(mkdtemp(path) != nullptr);
        root = path;
    }
    ~Bus() { nftw(root.c_str(), removeEntry, 8, FTW_DEPTH | FTW_PHYS); }

    void put(const std::string& folder, const char* name, const std::string& text) {
        const std::string path = root + "/" + folder;
        mkdir(path.c_str(), 0700);
        FILE* f = fopen((path + "/" + name).c_str(), "w");
        CHECK(f != nullptr);
        fputs((text + "\n").c_str(), f);
        fclose(f);
    }
    static std::string number(const char* format, unsigned value) {
        char text[16];
        snprintf(text, sizeof(text), format, value);
        return text;
    }
    // Unplugged: the device and its interfaces go.
    void remove(const std::string& port) {
        std::vector<std::string> gone;
        DIR* dir = opendir(root.c_str());
        CHECK(dir != nullptr);
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == port || name.compare(0, port.size() + 1, port + ":") == 0) {
                gone.push_back(name);
            }
        }
        closedir(dir);
        for (size_t i = 0; i < gone.size(); ++i) {
            nftw((root + "/" + gone[i]).c_str(), removeEntry, 8, FTW_DEPTH | FTW_PHYS);
        }
    }
    // A device with one interface of that class.
    void device(const std::string& port, unsigned bus, unsigned address, unsigned vendor, unsigned product,
                unsigned deviceClass, unsigned interfaceClass) {
        remove(port);
        put(port, "busnum", number("%u", bus));
        put(port, "devnum", number("%u", address));
        put(port, "idVendor", number("%04x", vendor));
        put(port, "idProduct", number("%04x", product));
        put(port, "bDeviceClass", number("%02x", deviceClass));
        put(port + ":1.0", "bInterfaceClass", number("%02x", interfaceClass));
        put(port + ":1.0", "bInterfaceSubClass", "01");
    }
    // A phone in accessory mode: interface 0 with its two bulk endpoints, as the kernel shows them.
    void accessory(const std::string& port, unsigned bus, unsigned address) {
        device(port, bus, address, 0x18d1, 0x2d01, 0, 0xff);
        const std::string interface = port + ":1.0";
        put(interface, "bInterfaceSubClass", "ff");
        mkdir((root + "/" + interface + "/ep_81").c_str(), 0700);
        put(interface + "/ep_81", "bEndpointAddress", "81");
        put(interface + "/ep_81", "bmAttributes", "02");
        put(interface + "/ep_81", "wMaxPacketSize", "0200");
        mkdir((root + "/" + interface + "/ep_01").c_str(), 0700);
        put(interface + "/ep_01", "bEndpointAddress", "01");
        put(interface + "/ep_01", "bmAttributes", "02");
        put(interface + "/ep_01", "wMaxPacketSize", "0200");
    }
};

struct Lock {
    pthread_mutex_t& mutex;
    explicit Lock(pthread_mutex_t& m) : mutex(m) { pthread_mutex_lock(&mutex); }
    ~Lock() { pthread_mutex_unlock(&mutex); }
};

// A phone: what the tests make it do, and what the code under test did to it. With a bus, it moves on it as a real
// phone does: to accessory mode when told to start, back to its own identity when reset.
struct Phone {
    pthread_mutex_t mu;
    int wake[2];
    Bus* bus;
    std::string port;
    unsigned busNumber, address, vendor, product;
    bool android;    // answers the accessory protocol request
    bool held;       // another program holds interface 0
    bool unplugged;  // the device left: transfers never come back
    bool stuckOut;   // takes no data
    unsigned opens, protocolAsked, started, resets, claims, releases;
    std::vector<std::string> strings;
    std::deque<std::vector<uint8_t>> toCar;     // what the phone sends, one transfer each
    std::vector<std::vector<uint8_t>> fromCar;  // each transfer received

    Phone()
        : bus(nullptr),
          busNumber(1),
          address(2),
          vendor(0x04e8),
          product(0x6860),
          android(true),
          held(false),
          unplugged(false),
          stuckOut(false),
          opens(0),
          protocolAsked(0),
          started(0),
          resets(0),
          claims(0),
          releases(0) {
        pthread_mutex_init(&mu, nullptr);
        CHECK(pipe2(wake, O_NONBLOCK | O_CLOEXEC) == 0);
    }
    ~Phone() {
        close(wake[0]);
        close(wake[1]);
        pthread_mutex_destroy(&mu);
    }
    void notify() {
        const char byte = 0;
        if (write(wake[1], &byte, 1) < 0) {
            // Full: the reader is already told.
        }
    }
    void plug(Bus& b, const std::string& p, bool asAccessory = false) {
        bus = &b;
        port = p;
        if (asAccessory) {
            bus->accessory(port, busNumber, address);
        } else {
            bus->device(port, busNumber, address, vendor, product, 0, 6);
        }
    }
    void say(const std::vector<uint8_t>& bytes) {
        Lock lock(mu);
        toCar.push_back(bytes);
        notify();
    }
    void set(bool Phone::*flag, bool value) {
        Lock lock(mu);
        this->*flag = value;
        notify();
    }
    unsigned count(unsigned Phone::*counter) {
        Lock lock(mu);
        return this->*counter;
    }
    std::vector<std::vector<uint8_t>> received() {
        Lock lock(mu);
        return fromCar;
    }
};

class Device : public evo::UsbDevice {
public:
    explicit Device(Phone& phone) : phone_(phone) {
        Lock lock(phone_.mu);
        ++phone_.opens;
    }
    int control(uint8_t type, uint8_t request, uint16_t, uint16_t index, uint8_t* data, uint16_t size,
                unsigned) override {
        Lock lock(phone_.mu);
        if (request == 51) {
            ++phone_.protocolAsked;
            if (!phone_.android || type != 0xc0 || size != 2) {
                return -EPIPE;
            }
            data[0] = 2;
            data[1] = 0;
            return 2;
        }
        if (request == 52) {
            CHECK(type == 0x40 && index == phone_.strings.size() && size > 0 && data[size - 1] == 0);
            phone_.strings.push_back(reinterpret_cast<const char*>(data));
            return size;
        }
        CHECK(request == 53 && type == 0x40 && size == 0);
        ++phone_.started;
        if (phone_.bus) {
            phone_.bus->accessory(phone_.port, phone_.busNumber, ++phone_.address);
        }
        return -ENODEV;  // as when the phone leaves before it acknowledges
    }
    bool claimInterface(unsigned number) override {
        Lock lock(phone_.mu);
        CHECK(number == 0);
        ++phone_.claims;
        if (phone_.held) {
            errno = EBUSY;
            return false;
        }
        return true;
    }
    void releaseInterface(unsigned number) override {
        Lock lock(phone_.mu);
        CHECK(number == 0);
        ++phone_.releases;
    }
    bool reset() override {
        Lock lock(phone_.mu);
        ++phone_.resets;
        phone_.strings.clear();
        if (phone_.bus) {
            phone_.bus->device(phone_.port, phone_.busNumber, ++phone_.address, phone_.vendor, phone_.product, 0, 6);
        }
        return true;
    }
    bool submit(evo::UsbTransfer* transfer) override {
        Lock lock(phone_.mu);
        if (phone_.unplugged) {
            return false;
        }
        pending_.push_back(transfer);
        phone_.notify();
        return true;
    }
    evo::UsbTransfer* reap() override {
        Lock lock(phone_.mu);
        char bytes[64];
        while (read(phone_.wake[0], bytes, sizeof(bytes)) > 0) {
        }
        if (phone_.unplugged) {
            return nullptr;
        }
        for (size_t i = 0; i < pending_.size(); ++i) {
            evo::UsbTransfer* transfer = pending_[i];
            if (cancelled_.count(transfer)) {
                cancelled_.erase(transfer);
                transfer->status = -ENOENT;
                transfer->done = 0;
            } else if (transfer->endpoint & 0x80) {
                if (phone_.toCar.empty()) {
                    continue;
                }
                const std::vector<uint8_t> bytesIn = phone_.toCar.front();
                phone_.toCar.pop_front();
                CHECK(bytesIn.size() <= transfer->data.size());
                memcpy(&transfer->data[0], bytesIn.data(), bytesIn.size());
                transfer->done = bytesIn.size();
                transfer->status = 0;
            } else {
                if (phone_.stuckOut) {
                    continue;
                }
                phone_.fromCar.push_back(transfer->data);
                transfer->done = transfer->data.size();
                transfer->status = 0;
            }
            pending_.erase(pending_.begin() + i);
            phone_.notify();
            return transfer;
        }
        return nullptr;
    }
    void cancel(evo::UsbTransfer* transfer) override {
        Lock lock(phone_.mu);
        cancelled_.insert(transfer);
        phone_.notify();
    }
    bool gone() const override {
        Lock lock(phone_.mu);
        return phone_.unplugged;
    }
    int waitFd() const override { return phone_.wake[0]; }
    short waitEvents() const override { return POLLIN; }

private:
    Phone& phone_;
    std::vector<evo::UsbTransfer*> pending_;
    std::set<evo::UsbTransfer*> cancelled_;
};

// True once `done()` is, within the time given.
template <typename Done>
bool within(unsigned ms, Done done) {
    const uint32_t begin = nowMs();
    while (!done()) {
        if (nowMs() - begin >= ms) {
            return false;
        }
        usleep(5000);
    }
    return true;
}

}  // namespace fake

#endif
