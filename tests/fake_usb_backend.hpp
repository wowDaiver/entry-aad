// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Fake USB bus with an optional socket-connected AA phone. No real device access.
#ifndef ENTRY_AAD_FAKE_USB_BACKEND_HPP
#define ENTRY_AAD_FAKE_USB_BACKEND_HPP
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include "usb_transport.hpp"

struct FakeUsbStats {
    unsigned opens, closes, claims, releases, strings, starts, bulkReads, bulkWrites;
    FakeUsbStats() : opens(0), closes(0), claims(0), releases(0), strings(0), starts(0), bulkReads(0), bulkWrites(0) {}
};
struct FakeBulk {
    int rc, count;
    std::vector<uint8_t> bytes;
    FakeBulk(int r, int n) : rc(r), count(n), bytes(n > 0 ? n : 0, 0x5a) {}
};
struct FakeUsbTree {
    std::string root;
    FakeUsbTree() {
        char path[] = "/tmp/entry-aad-usb-test-XXXXXX";
        const char* created = mkdtemp(path);
        assert(created);
        root = created;
        assert(mkdir((root + "/3-1").c_str(), 0700) == 0);
        identity(false);
    }
    ~FakeUsbTree() {
        const char* names[] = {"busnum", "devnum", "idVendor", "idProduct", "bDeviceClass"};
        for (unsigned i = 0; i < 5; ++i) {
            unlink((root + "/3-1/" + names[i]).c_str());
        }
        rmdir((root + "/3-1:1.0").c_str());
        rmdir((root + "/3-1").c_str());
        rmdir(root.c_str());
    }
    void field(const char* name, const char* value) {
        FILE* f = fopen((root + "/3-1/" + name).c_str(), "w");
        assert(f);
        fputs(value, f);
        fclose(f);
    }
    void identity(bool accessory) {
        if (accessory) {
            mkdir((root + "/3-1:1.0").c_str(), 0700);
        } else {
            rmdir((root + "/3-1:1.0").c_str());
        }
        field("busnum", "3\n");
        field("devnum", accessory ? "3\n" : "2\n");
        field("idVendor", accessory ? "18d1\n" : "04e8\n");
        field("idProduct", accessory ? "2d01\n" : "6860\n");
        field("bDeviceClass", "00\n");
    }
    aa::UsbSelection selection(bool adopt = false) const {
        aa::UsbSelection s;
        s.port = "3-1";
        s.vendor = 0x04e8;
        s.product = 0x6860;
        s.adoptAccessory = adopt;
        return s;
    }
};

class FakeUsbBackend : public aa::UsbBackend {
public:
    FakeUsbBackend(FakeUsbTree& tree, FakeUsbStats& stats, int socket = -1)
        : tree(tree),
          stats(stats),
          fd(socket),
          opened(false),
          accessory(false),
          mismatch(false),
          reenumerate(true),
          claimRc(0),
          protocolSize(2),
          protocolVersion(2),
          startRc(0),
          failString(-1),
          activeConfig(1),
          seizeBeforeConfig(false),
          quit(nullptr),
          cancelRequest(-1) {
        // Accessory + ADB. Deliberately distinct endpoint addresses for ADB.
        const uint8_t raw[] = {9,    2,    55, 0, 2, 1,    0, 0x80, 50, 9, 4, 0, 0, 2, 0xff, 0xff, 0, 0, 7,
                               5,    0x81, 2,  0, 2, 0,    7, 5,    2,  2, 0, 2, 0, 9, 4,    1,    0, 2, 0xff,
                               0x42, 1,    0,  7, 5, 0x83, 2, 0,    2,  0, 7, 5, 4, 2, 0,    2,    0};
        config.assign(raw, raw + sizeof(raw));
    }
    ~FakeUsbBackend() override {
        close();
        if (fd >= 0) {
            ::close(fd);
        }
    }
    bool open(unsigned bus, unsigned address) override {
        assert(bus == 3);
        assert(address == 2 || address == 3);
        ++stats.opens;
        opened = true;
        accessory = address == 3;
        return true;
    }
    void close() override {
        if (opened) {
            opened = false;
            ++stats.closes;
        }
    }
    int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t size,
                unsigned timeout) override {
        assert(opened && timeout > 0 && timeout <= 250);
        if (quit && request == cancelRequest) {
            *quit = 1;
        }
        if (request == 6 && value == 0x100) {
            assert(type == 0x80 && size == 18);
            memset(data, 0, size);
            data[0] = 18;
            data[1] = 1;
            unsigned vid = accessory ? 0x18d1 : 0x04e8, pid = accessory ? 0x2d01 : 0x6860;
            if (mismatch) {
                ++pid;
            }
            data[8] = vid;
            data[9] = vid >> 8;
            data[10] = pid;
            data[11] = pid >> 8;
            return 18;
        }
        if (request == 6 && value == 0x200) {
            if (seizeBeforeConfig && stats.claims == 0) {
                claimRc = -6;
            }
            assert(type == 0x80 && accessory);
            const size_t n = std::min(static_cast<size_t>(size), config.size());
            memcpy(data, &config[0], n);
            return n;
        }
        if (request == 8) {
            assert(type == 0x80);
            *data = activeConfig;
            return 1;
        }
        if (request == 51) {
            assert(type == 0xc0 && !accessory && size == 2);
            data[0] = protocolVersion;
            data[1] = 0;
            return protocolSize;
        }
        if (request == 52) {
            assert(type == 0x40 && value == 0 && index == stats.strings && data[size - 1] == 0);
            ++stats.strings;
            return index == failString ? -1 : size;
        }
        if (request == 53) {
            assert(type == 0x40 && !size && stats.strings == 6);
            ++stats.starts;
            if (reenumerate) {
                tree.identity(true);
            }
            return startRc;
        }
        assert(false);
        return -99;
    }
    int claim(unsigned interface) override {
        assert(opened && accessory && interface == 0);
        ++stats.claims;
        return claimRc;
    }
    int release(unsigned interface) override {
        assert(interface == 0 && !claimRc);
        ++stats.releases;
        return 0;
    }
    int bulk(uint8_t endpoint, uint8_t* data, int size, int& transferred, unsigned timeout) override {
        const bool input = endpoint & 0x80;
        assert(endpoint == 0x81 || endpoint == 2);  // ADB must never be touched
        if (input) {
            ++stats.bulkReads;
            assert(size == 16384 && timeout <= 20);
        } else {
            ++stats.bulkWrites;
            assert(timeout <= 100);
        }
        std::deque<FakeBulk>& queue = input ? reads : writes;
        if (!queue.empty()) {
            const FakeBulk action = queue.front();
            queue.pop_front();
            transferred = action.count;
            if (action.count > 0 && action.count <= size) {
                if (input) {
                    memcpy(data, &action.bytes[0], action.count);
                } else {
                    sent.insert(sent.end(), data, data + action.count);
                }
            }
            return action.rc;
        }
        transferred = 0;
        if (!input && size == 0) {
            return 0;
        }
        if (fd < 0) {
            return -7;
        }
        if (!input) {
            // Force short writes and timeout-with-progress even throughout TLS handshake.
            transferred = ::send(fd, data, std::min(size, 31), 0);
            return transferred > 0 ? -7 : -4;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        timeval wait = {0, static_cast<int>(timeout * 1000)};
        if (select(fd + 1, &readable, nullptr, nullptr, &wait) <= 0) {
            return -7;
        }
        transferred = recv(fd, data, size, 0);
        return transferred > 0 ? 0 : -4;
    }
    FakeUsbTree& tree;
    FakeUsbStats& stats;
    int fd;
    bool opened, accessory, mismatch, reenumerate;
    int claimRc, protocolSize, protocolVersion, startRc, failString, activeConfig;
    bool seizeBeforeConfig;
    volatile sig_atomic_t* quit;
    int cancelRequest;
    std::vector<uint8_t> config, sent;
    std::deque<FakeBulk> reads, writes;
};
#endif
