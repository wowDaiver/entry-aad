// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Independently implemented from the AOA specification; no code from the GPL wired prototype.
#include "usb_transport.hpp"

#include <fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

namespace aa {
namespace {
const int kTimeout = -7;  // public libusb_error ABI
const int kNoDevice = -4;
const unsigned kControlMs = 250;
const unsigned kReadMs = 20;
const unsigned kWriteMs = 100;

uint64_t clockMs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
bool stopped(const volatile sig_atomic_t* quit) {
    return quit && *quit;
}
unsigned le16(const uint8_t* p) {
    return p[0] | (static_cast<unsigned>(p[1]) << 8);
}

bool number(const std::string& directory, const char* name, int base, unsigned& value) {
    FILE* f = fopen((directory + "/" + name).c_str(), "r");
    if (!f) {
        return false;
    }
    char text[32];
    const bool got = fgets(text, sizeof(text), f) != nullptr;
    fclose(f);
    if (!got || !*text || *text == '-') {
        return false;
    }
    char* end;
    const unsigned long n = strtoul(text, &end, base);
    if (end == text || n > 65535) {
        return false;
    }
    while (*end == '\n' || *end == '\r') {
        ++end;
    }
    if (*end) {
        return false;
    }
    value = static_cast<unsigned>(n);
    return true;
}

bool fail(const char* stage, int result) {
    printf("[USB] stage=%s result=%d; no reset, detach or service stop\n", stage, result);
    return false;
}

bool verifyDevice(UsbBackend& io, const UsbIdentity& id) {
    uint8_t d[18] = {};
    const int n = io.deviceDescriptor(d);
    return (n == 18 && d[0] == 18 && d[1] == 1 && d[4] != 9 && le16(d + 8) == id.vendor &&
            le16(d + 10) == id.product) ||
           fail("device-descriptor", n);
}

// Select only interface 0/alternate 0, never the second (ADB) bulk pair.
bool endpoints(UsbBackend& io, uint8_t& input, uint8_t& output, unsigned& outputPacket) {
    uint8_t header[9] = {};
    int n = io.control(0x80, 6, 0x0200, 0, header, sizeof(header), kControlMs);
    if (n != 9 || header[0] != 9 || header[1] != 2 || header[5] != 1) {
        return fail("config-header", n);
    }
    const unsigned total = le16(header + 2);
    if (total < 9 || total > 4096) {
        return fail("config-size", total);
    }
    uint8_t config[4096] = {};
    n = io.control(0x80, 6, 0x0200, 0, config, total, kControlMs);
    if (n != static_cast<int>(total) || memcmp(header, config, 9)) {
        return fail("config-descriptor", n);
    }
    bool selected = false;
    unsigned seen = 0, count = 0, unique = 0;
    bool interfaces[256] = {};
    input = output = 0;
    for (unsigned p = 9; p < total;) {
        if (p + 2 > total || config[p] < 2 || p + config[p] > total) {
            return fail("descriptor-length", p);
        }
        const uint8_t* d = config + p;
        if (d[1] == 4) {
            if (d[0] != 9) {
                return fail("interface-length", d[0]);
            }
            if (!interfaces[d[2]]) {
                interfaces[d[2]] = true;
                ++unique;
            }
            selected = d[2] == 0 && d[3] == 0;
            if (selected && (++seen != 1 || d[4] != 2 || d[5] != 0xff || d[6] != 0xff || d[7] != 0)) {
                return fail("accessory-interface", d[2]);
            }
        } else if (d[1] == 5 && selected) {
            if (d[0] < 7 || (d[3] & 3) != 2 || !(d[2] & 15) || (d[2] & 0x70) || !le16(d + 4) || le16(d + 4) > 512) {
                return fail("bulk-endpoint", p);
            }
            uint8_t& endpoint = (d[2] & 0x80) ? input : output;
            if (endpoint) {
                return fail("duplicate-endpoint", d[2]);
            }
            endpoint = d[2];
            if (!(d[2] & 0x80)) {
                outputPacket = le16(d + 4);
            }
            ++count;
        }
        p += d[0];
    }
    if (seen != 1 || count != 2 || !input || !output || unique != config[4]) {
        return fail("bulk-pair", count);
    }
    uint8_t active = 0;
    n = io.control(0x80, 8, 0, 0, &active, 1, kControlMs);
    // Linux normally configures the accessory automatically. Do not reset BMW/ADB interface ownership to fix it.
    return (n == 1 && active == 1) || fail("active-configuration-not-1", n);
}

class UsbTransport : public Transport {
public:
    explicit UsbTransport(UsbBackend* io)
        : io_(io), cancelled_(false), claimed_(false), input_(0), output_(0), offset_(0), size_(0), rx_(0), tx_(0) {}
    ~UsbTransport() override {
        if (claimed_) {
            const int rc = io_->release(0);
            printf("[USB] release interface=0 result=%d\n", rc);
        }
        io_->close();
        printf("[USB] closed rx=%llu tx=%llu\n", static_cast<unsigned long long>(rx_),
               static_cast<unsigned long long>(tx_));
    }
    bool reserveFreshAccessory() {
        // Only after our own AOA switch, with a fresh device on the same port and its descriptor verified.
        const int rc = io_->claim(0);
        if (rc) {
            return fail("claim-fresh-accessory-interface-0", rc);
        }
        claimed_ = true;
        printf("[USB] reserved fresh accessory interface=0 before config reads\n");
        return true;
    }
    bool prepare(const volatile sig_atomic_t* quit) {
        if (stopped(quit) || !endpoints(*io_, input_, output_, outputPacket_) || stopped(quit)) {
            return false;
        }
        if (!claimed_) {
            const int rc = io_->claim(0);
            if (rc) {
                return fail("claim-interface-0", rc);
            }
            claimed_ = true;
        }
        printf("[USB] bulk-ready interface=0 in=0x%02x out=0x%02x\n", input_, output_);
        return !stopped(quit);
    }
    ssize_t read(uint8_t* data, size_t size, int wakeFd, bool& woke) override {
        woke = wakeReady(wakeFd);
        if (cancelled_) {
            return -1;
        }
        // A continuous microphone/input wake must not starve IN; make bounded progress on both directions.
        if (offset_ == size_) {
            // A USB request for a 4-byte AA header can overflow a 512-byte packet. Always receive into a packet-
            // aligned staging buffer and retain the rest, including the following AA frames.
            int transferred = 0;
            const int rc = io_->bulk(input_, buffer_, sizeof(buffer_), transferred, kReadMs);
            if (transferred < 0 || transferred > static_cast<int>(sizeof(buffer_))) {
                return -1;
            }
            if (rc && rc != kTimeout) {
                fail("bulk-in", rc);
                return -1;
            }
            offset_ = 0;
            size_ = static_cast<size_t>(transferred);
            rx_ += size_;
            woke = woke || wakeReady(wakeFd);
        }
        const size_t count = std::min(size, size_ - offset_);
        if (count) {
            memcpy(data, buffer_ + offset_, count);
        }
        offset_ += count;
        return cancelled_ ? -1 : static_cast<ssize_t>(count);
    }
    ssize_t write(const uint8_t* data, size_t size) override {
        if (cancelled_) {
            return -1;
        }
        const int count = static_cast<int>(std::min(size, sizeof(buffer_)));
        int transferred = 0;
        const int rc = io_->bulk(output_, const_cast<uint8_t*>(data), count, transferred, kWriteMs);
        if (transferred < 0 || transferred > count) {
            return -1;
        }
        if (rc && rc != kTimeout) {
            fail("bulk-out", rc);
            return -1;
        }
        if (!rc && !transferred) {
            fail("bulk-out-zero-progress", 0);
            return -1;
        }
        // A timeout may have transferred bytes. Return that progress so the session never resends a prefix.
        if (transferred) {
            lastWrite_ = transferred;
        }
        tx_ += transferred;
        return cancelled_ ? -1 : transferred;
    }
    bool endWrite() override {
        if (cancelled_) {
            return false;
        }
        if (lastWrite_ && outputPacket_ && lastWrite_ % outputPacket_ == 0) {
            // Android's accessory gadget can have a larger pending OUT request. A short packet (ZLP here) ends it
            // even when the AA frame itself is an exact packet-size multiple. It adds no bytes to the AA stream.
            int transferred = 0;
            uint8_t unused = 0;
            const int rc = io_->bulk(output_, &unused, 0, transferred, kWriteMs);
            if (rc || transferred) {
                return fail("bulk-out-zlp", rc);
            }
        }
        lastWrite_ = 0;
        return !cancelled_;
    }
    void cancel() override { cancelled_ = true; }  // at most one bounded synchronous bulk transfer remains
    const char* name() const override { return "USB/AOA"; }
    UsbBackend& io() { return *io_; }

private:
    static bool wakeReady(int fd) {
        if (fd < 0 || fd >= FD_SETSIZE) {
            return false;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        timeval wait = {0, 0};
        return select(fd + 1, &fds, nullptr, nullptr, &wait) > 0;
    }
    std::unique_ptr<UsbBackend> io_;
    std::atomic<bool> cancelled_;
    bool claimed_;
    uint8_t input_, output_, buffer_[16384];
    unsigned outputPacket_, lastWrite_;
    size_t offset_, size_;
    uint64_t rx_, tx_;
};
}  // namespace

bool UsbIdentity::accessory() const {
    return vendor == 0x18d1 && (product == 0x2d00 || product == 0x2d01);
}
bool UsbSelection::valid() const {
    if (!vendor || vendor > 65535 || !product || product > 65535 ||
        (vendor == 0x18d1 && (product == 0x2d00 || product == 0x2d01)) || port.size() > 64) {
        return false;
    }
    // Linux topology spelling: bus-port[.port...], each positive. No traversal, interface suffix or root hub.
    const char* p = port.c_str();
    unsigned fields = 0;
    while (*p) {
        if (*p < '1' || *p > '9') {
            return false;
        }
        unsigned value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p++ - '0');
            if (value > 255) {
                return false;
            }
        }
        ++fields;
        if (!*p) {
            break;
        }
        if (*p++ != (fields == 1 ? '-' : '.')) {
            return false;
        }
        if (!*p) {
            return false;
        }
    }
    return fields >= 2;
}
int UsbSelection::loadFile(const char* path) {
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 022) || st.st_size > 128) {
        close(fd);
        return -1;
    }
    char text[129] = {};
    const ssize_t length = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (length < 0 || length != st.st_size || memchr(text, 0, length)) {
        return -1;
    }
    const char* values = text;
    while (*values == ' ' || *values == '\t' || *values == '\n' || *values == '\r') {
        ++values;
    }
    UsbSelection candidate;
    if (!strncmp(values, "auto", 4) && (values[4] == ' ' || values[4] == '\t')) {
        candidate.automatic = true;
        values += 4;
    }
    char portText[65], extra;
    unsigned vid = 0, pid = 0;
    if (sscanf(values, "%64s %x %x %c", portText, &vid, &pid, &extra) != 3) {
        return -1;
    }
    candidate.port = portText;
    candidate.vendor = vid;
    candidate.product = pid;
    if (!candidate.valid()) {
        return -1;
    }
    *this = candidate;
    return 1;
}
bool readUsbIdentity(const UsbSelection& selection, UsbIdentity& id, const char* sysfs) {
    if (!selection.valid()) {
        return false;
    }
    const std::string directory = std::string(sysfs) + "/" + selection.port;
    return number(directory, "busnum", 10, id.bus) && id.bus > 0 && id.bus <= 255 &&
           number(directory, "devnum", 10, id.address) && id.address > 0 && id.address <= 127 &&
           number(directory, "idVendor", 16, id.vendor) && number(directory, "idProduct", 16, id.product) &&
           number(directory, "bDeviceClass", 16, id.deviceClass);
}
bool UsbRetryBudget::take(const UsbIdentity& id) {
    if (bus_ != id.bus || address_ != id.address) {
        bus_ = id.bus;
        address_ = id.address;
        attempts_ = 0;
    }
    if (attempts_ >= 3) {
        return false;
    }
    ++attempts_;
    return true;
}
void UsbRetryBudget::followReenumeration(const UsbIdentity& id) {
    bus_ = id.bus;
    address_ = id.address;
}

Transport* connectUsb(const UsbSelection& selection, const volatile sig_atomic_t* quit, UsbBackend* backend,
                      const char* sysfs) {
    if (!backend) {
        return nullptr;
    }
    std::unique_ptr<UsbTransport> stream(new UsbTransport(backend));
    UsbBackend& io = stream->io();
    UsbIdentity initial;
    if (stopped(quit) || !readUsbIdentity(selection, initial, sysfs) || initial.deviceClass == 9) {
        return nullptr;
    }
    if (!(initial.vendor == selection.vendor && initial.product == selection.product) &&
        !(selection.adoptAccessory && initial.accessory())) {
        fail("identity-mismatch", 0);
        return nullptr;
    }
    printf("[USB] selected port=%s bus=%u address=%u vid=%04x pid=%04x\n", selection.port.c_str(), initial.bus,
           initial.address, initial.vendor, initial.product);
    if (!io.open(initial.bus, initial.address) || stopped(quit) || !verifyDevice(io, initial)) {
        return nullptr;
    }
    UsbIdentity checked;
    if (!readUsbIdentity(selection, checked, sysfs) || checked.bus != initial.bus ||
        checked.address != initial.address || checked.vendor != initial.vendor || checked.product != initial.product) {
        return nullptr;
    }
    if (!initial.accessory()) {
        uint8_t version[2] = {};
        int n = io.control(0xc0, 51, 0, 0, version, 2, kControlMs);
        if (n != 2 || (le16(version) != 1 && le16(version) != 2)) {
            fail("aoa-protocol", n);
            return nullptr;
        }
        printf("[USB] AOA protocol=%u\n", le16(version));
        // Android Auto's accessory identity. This is AA's multiplexed protocol, not deprecated AOA USB audio/HID.
        const char* strings[] = {"Android", "Android Auto", "entry-aad USB", "1.0", "", "entry-aad-usb"};
        for (unsigned i = 0; i < 6; ++i) {
            if (stopped(quit)) {
                return nullptr;
            }
            const size_t len = strlen(strings[i]) + 1;
            n = io.control(0x40, 52, 0, i, reinterpret_cast<uint8_t*>(const_cast<char*>(strings[i])), len, kControlMs);
            if (n != static_cast<int>(len)) {
                fail("aoa-string", n);
                return nullptr;
            }
        }
        if (stopped(quit)) {
            return nullptr;
        }
        n = io.control(0x40, 53, 0, 0, nullptr, 0, kControlMs);
        io.close();
        // A detach can race completion of ACCESSORY_START. Only the expected fresh device proves success.
        if (n != 0 && n != kNoDevice) {
            fail("aoa-start", n);
            return nullptr;
        }
        const uint64_t deadline = clockMs() + 15000;
        bool found = false;
        while (!stopped(quit) && clockMs() < deadline) {
            UsbIdentity next;
            struct stat interfaceState;
            const std::string interfacePath = std::string(sysfs) + "/" + selection.port + ":1.0";
            if (readUsbIdentity(selection, next, sysfs) && next.bus == initial.bus && next.address != initial.address &&
                next.accessory() && stat(interfacePath.c_str(), &interfaceState) == 0) {
                if (!io.open(next.bus, next.address) || !verifyDevice(io, next)) {
                    return nullptr;
                }
                if (!readUsbIdentity(selection, checked, sysfs) || checked.bus != next.bus ||
                    checked.address != next.address || !checked.accessory()) {
                    return nullptr;
                }
                if (stopped(quit) || !stream->reserveFreshAccessory()) {
                    return nullptr;
                }
                printf("[USB] re-enumerated same port=%s bus=%u address=%u vid=%04x pid=%04x\n", selection.port.c_str(),
                       next.bus, next.address, next.vendor, next.product);
                found = true;
                break;
            }
            usleep(clockMs() + 14000 < deadline ? 1000 : 10000);
        }
        if (!found) {
            fail("aoa-reenumeration", stopped(quit) ? -10 : kTimeout);
            return nullptr;
        }
    }
    if (!stream->prepare(quit)) {
        return nullptr;
    }
    return stream.release();
}
}  // namespace aa
