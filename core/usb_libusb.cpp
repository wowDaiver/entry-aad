// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Old libusb 1.0 ABI, loaded from the head unit. Only public opaque types/function signatures are declared here.
#include <dlfcn.h>

#include <cstdio>
#include <cstring>

#include "usb_transport.hpp"

namespace aa {
namespace {
struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

// Public libusb device-descriptor ABI. No native library structures are accessed through opaque handles.
struct DeviceDescriptor {
    uint8_t length, type;
    uint16_t usbVersion;
    uint8_t deviceClass, subclass, protocol, packetSize;
    uint16_t vendor, product, deviceVersion;
    uint8_t manufacturer, model, serial, configurations;
};
static_assert(sizeof(DeviceDescriptor) == 18 && offsetof(DeviceDescriptor, vendor) == 8,
              "libusb standard descriptor ABI");

class LibUsb : public UsbBackend {
public:
    LibUsb() : library_(nullptr), context_(nullptr), handle_(nullptr), device_(nullptr) {}
    ~LibUsb() override {
        close();
        if (context_) {
            exit_(context_);
        }
        if (library_) {
            dlclose(library_);
        }
    }
    bool init() {
        library_ = dlopen("libusb-1.0.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library_) {
            printf("[USB] installed libusb-1.0.so.0 unavailable\n");
            return false;
        }
#define USB_BIND(member, symbol)                                          \
    member = reinterpret_cast<decltype(member)>(dlsym(library_, symbol)); \
    if (!member)                                                          \
    return false
        USB_BIND(init_, "libusb_init");
        USB_BIND(exit_, "libusb_exit");
        USB_BIND(list_, "libusb_get_device_list");
        USB_BIND(free_, "libusb_free_device_list");
        USB_BIND(bus_, "libusb_get_bus_number");
        USB_BIND(address_, "libusb_get_device_address");
        USB_BIND(open_, "libusb_open");
        USB_BIND(descriptor_, "libusb_get_device_descriptor");
        USB_BIND(close_, "libusb_close");
        USB_BIND(control_, "libusb_control_transfer");
        USB_BIND(claim_, "libusb_claim_interface");
        USB_BIND(release_, "libusb_release_interface");
        USB_BIND(bulk_, "libusb_bulk_transfer");
#undef USB_BIND
        const int rc = init_(&context_);
        if (rc) {
            context_ = nullptr;
            printf("[USB] libusb_init result=%d\n", rc);
        }
        return rc == 0;
    }
    bool open(unsigned bus, unsigned address) override {
        close();
        libusb_device** list = nullptr;
        const ssize_t count = list_(context_, &list);
        if (count < 0) {
            printf("[USB] enumerate result=%d\n", static_cast<int>(count));
            return false;
        }
        for (ssize_t i = 0; i < count; ++i) {
            if (bus_(list[i]) == bus && address_(list[i]) == address) {
                const int rc = open_(list[i], &handle_);
                if (!rc) {
                    device_ = list[i];  // the open handle retains its device reference
                }
                if (rc) {
                    printf("[USB] open selected device result=%d\n", rc);
                }
                break;
            }
        }
        free_(list, 1);
        return handle_ != nullptr;
    }
    void close() override {
        if (handle_) {
            close_(handle_);
            handle_ = nullptr;
            device_ = nullptr;
        }
    }
    int deviceDescriptor(uint8_t* data) override {
        DeviceDescriptor d = {};
        const int rc = descriptor_(device_, &d);
        if (rc) {
            return rc;
        }
        memcpy(data, &d, sizeof(d));
        // libusb's 16-bit fields are host-endian; the core consumes a wire-format descriptor.
        const uint16_t words[] = {d.usbVersion, d.vendor, d.product, d.deviceVersion};
        const unsigned offsets[] = {2, 8, 10, 12};
        for (unsigned i = 0; i < 4; ++i) {
            data[offsets[i]] = words[i] & 255;
            data[offsets[i] + 1] = words[i] >> 8;
        }
        return sizeof(d);
    }
    int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t size,
                unsigned ms) override {
        return control_(handle_, type, request, value, index, data, size, ms);
    }
    int claim(unsigned interface) override { return claim_(handle_, interface); }
    int release(unsigned interface) override { return release_(handle_, interface); }
    int bulk(uint8_t endpoint, uint8_t* data, int size, int& transferred, unsigned ms) override {
        return bulk_(handle_, endpoint, data, size, &transferred, ms);
    }

private:
    void* library_;
    libusb_context* context_;
    libusb_device_handle* handle_;
    libusb_device* device_;
    int (*init_)(libusb_context**);
    void (*exit_)(libusb_context*);
    ssize_t (*list_)(libusb_context*, libusb_device***);
    void (*free_)(libusb_device**, int);
    uint8_t (*bus_)(libusb_device*);
    uint8_t (*address_)(libusb_device*);
    int (*open_)(libusb_device*, libusb_device_handle**);
    void (*close_)(libusb_device_handle*);
    int (*descriptor_)(libusb_device*, DeviceDescriptor*);
    int (*control_)(libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t, uint8_t*, uint16_t, unsigned);
    int (*claim_)(libusb_device_handle*, int);
    int (*release_)(libusb_device_handle*, int);
    int (*bulk_)(libusb_device_handle*, uint8_t, uint8_t*, int, int*, unsigned);
};
}  // namespace
UsbBackend* libusbBackend() {
    LibUsb* io = new LibUsb;
    if (io->init()) {
        return io;
    }
    delete io;
    return nullptr;
}
}  // namespace aa
