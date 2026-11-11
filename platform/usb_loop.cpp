// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The USB phone loop (see usb_loop.hpp).
#include "usb_loop.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "evo_log.hpp"
#include "usb_accessory.hpp"
#include "usb_bridge.hpp"

namespace evo {
namespace {

const unsigned kMaxResets = 3;        // per phone, until a session held or the phone is plugged in again
const uint32_t kHeldMs = 30000;       // a session that lasted this long worked
const unsigned kMaxSilentTries = 24;  // sessions in a row the phone said nothing in (5 s each) before a reset
const unsigned kMaxClaimTries = 10;   // turns in a row its interface was held or not ready
const unsigned kMaxAskFailures = 3;   // switch requests that failed half-way
const uint32_t kUnpluggedMs = 6000;   // no phone on the bus for this long: it was unplugged
const uint32_t kReturnMs = 5000;      // how long the return as an accessory is watched closely
const unsigned kReturnStepUs = 20000;

class Loop {
public:
    Loop(const UsbLoopSetup& setup, UsbHost& host)
        : setup_(setup),
          host_(host),
          bridge_(nullptr),
          bridgeStart_(0),
          resets_(0),
          silent_(0),
          claimTries_(0),
          askFailures_(0),
          quietSince_(0),
          quiet_(false),
          emptySince_(0),
          empty_(false) {}

    void run() {
        ELOG("UsbLoop", "watching %s%s for an Android phone", setup_.port.empty() ? "every USB port" : "USB port ",
             setup_.port.c_str());
        bool atOnce = false;
        while (!host_.stopRequested()) {
            if (!atOnce) {
                rest(setup_.turnMs);
            }
            atOnce = turn();
        }
        leave();
    }

private:
    // Sleeps, but not past a stop request.
    void rest(unsigned ms) {
        for (unsigned waited = 0; waited < ms && !host_.stopRequested(); waited += 50) {
            usleep(50000);
        }
    }

    // One look at the bus. True when the next one must follow at once.
    bool turn() {
        if (bridge_) {
            if (bridge_->finished()) {
                bridgeEnded();
            }
            return false;
        }
        if (host_.projectionBusy() || (quiet_ && monoMs() - quietSince_ < setup_.afterResetMs)) {
            return false;
        }
        quiet_ = false;
        std::vector<UsbPhone> phones;
        listUsbPhones(setup_.sysfs, setup_.port, phones);
        forgetGone();
        for (size_t i = 0; i < phones.size(); ++i) {
            if (!leftAlone(phones[i])) {
                noteEmpty(false);
                return phones[i].accessory ? useAccessory(phones[i]) : askPhone(phones[i]);
            }
        }
        noteEmpty(true);
        return false;
    }

    bool leftAlone(const UsbPhone& phone) const {
        const std::map<std::string, unsigned>::const_iterator it = leftAlone_.find(phone.port);
        return it != leftAlone_.end() && it->second == phone.address;
    }

    void leaveAlone(const UsbPhone& phone) { leftAlone_[phone.port] = phone.address; }

    // No phone for longer than a phone takes to come back after a switch or a reset: it was unplugged, and the
    // next one starts with every try allowed.
    void noteEmpty(bool empty) {
        if (!empty) {
            empty_ = false;
        } else if (!empty_) {
            empty_ = true;
            emptySince_ = monoMs();
        } else if (monoMs() - emptySince_ >= kUnpluggedMs) {
            ours_.clear();
            resets_ = silent_ = claimTries_ = askFailures_ = 0;
        }
    }

    // A device that was unplugged, or came back as another one, may be looked at again.
    void forgetGone() {
        for (std::map<std::string, unsigned>::iterator it = leftAlone_.begin(); it != leftAlone_.end();) {
            UsbPhone now;
            if (!readUsbDevice(setup_.sysfs, it->first, now) || now.address != it->second) {
                leftAlone_.erase(it++);
            } else {
                ++it;
            }
        }
    }

    void bridgeEnded() {
        const uint32_t lasted = monoMs() - bridgeStart_;
        const bool spoke = bridge_->bytesFromPhone() > 0;
        const bool left = bridge_->phoneLeft();
        delete bridge_;
        bridge_ = nullptr;
        ELOG("UsbLoop", "USB session over after %u s (%s)", lasted / 1000,
             left    ? "the phone left"
             : spoke ? "session ended"
                     : "the phone said nothing");
        if (lasted >= kHeldMs) {
            resets_ = 0;
        }
        if (spoke) {
            silent_ = 0;
            ours_.erase(bridgePort_);  // its Android Auto is over: only a fresh accessory starts a new one
        } else if (++silent_ >= kMaxSilentTries) {
            silent_ = 0;
            ours_.erase(bridgePort_);
        }
    }

    // An accessory on the bus: used when this loop just made it, else reset so that the phone can be asked again.
    bool useAccessory(const UsbPhone& phone) {
        if (!ours_.count(phone.port)) {
            return resetPhone(phone);
        }
        AccessoryEndpoints endpoints;
        UsbDevice* device = nullptr;
        const bool described = readAccessoryEndpoints(setup_.sysfs, phone.port, endpoints);
        if (described) {
            device = setup_.open(phone.bus, phone.address);
        }
        if (!device || !device->claimInterface(0)) {
            const int why = errno;
            delete device;
            if (++claimTries_ >= kMaxClaimTries) {
                claimTries_ = 0;
                ELOG("UsbLoop", "accessory on %s not available (%s): %s", phone.port.c_str(),
                     !described ? "not set up by the kernel" : strerror(why),
                     why == EBUSY ? "another program holds it, it is left to it" : "giving this one up");
                ours_.erase(phone.port);  // the next look resets the phone, within its limit
            }
            return false;
        }
        claimTries_ = 0;
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
            device->releaseInterface(0);
            delete device;
            return false;
        }
        bridge_ = new UsbBridge(device, endpoints, fds[0]);
        if (!bridge_->start()) {
            delete bridge_;
            bridge_ = nullptr;
            close(fds[1]);
            return false;
        }
        bridgePort_ = phone.port;
        bridgeStart_ = monoMs();
        ELOG("UsbLoop", "phone on %s (bus %u, address %u) in accessory mode: starting Android Auto", phone.port.c_str(),
             phone.bus, phone.address);
        host_.usbPhoneReady(fds[1]);
        return false;
    }

    bool resetPhone(const UsbPhone& phone) {
        if (resets_ >= kMaxResets) {
            ELOG("UsbLoop", "phone on %s still not starting Android Auto after %u resets: unplug it and plug it again",
                 phone.port.c_str(), resets_);
            leaveAlone(phone);
            return false;
        }
        ++resets_;
        UsbDevice* device = setup_.open(phone.bus, phone.address);
        const bool done = device && device->reset();
        const int why = errno;
        delete device;
        if (!done) {
            ELOG("UsbLoop", "phone on %s cannot be reset (%s): unplug it and plug it again", phone.port.c_str(),
                 strerror(why));
            leaveAlone(phone);
            return false;
        }
        ELOG("UsbLoop", "phone on %s was left in accessory mode: reset %u of %u, then it is asked again",
             phone.port.c_str(), resets_, kMaxResets);
        ours_.insert(phone.port);  // whatever comes back on this port is fresh
        quiet_ = true;
        quietSince_ = monoMs();
        return false;
    }

    bool askPhone(const UsbPhone& phone) {
        UsbDevice* device = setup_.open(phone.bus, phone.address);
        if (!device) {
            ELOG("UsbLoop", "device %04x:%04x on %s cannot be opened (%s): left alone", phone.vendor, phone.product,
                 phone.port.c_str(), strerror(errno));
            leaveAlone(phone);
            return false;
        }
        unsigned protocol = 0;
        const AccessorySwitch result = askAccessoryMode(*device, protocol);
        delete device;
        if (result == kSwitchNotAndroid) {
            ELOG("UsbLoop", "device %04x:%04x on %s is not an Android phone: left alone", phone.vendor, phone.product,
                 phone.port.c_str());
            leaveAlone(phone);
            return false;
        }
        if (result == kSwitchFailed) {
            if (++askFailures_ >= kMaxAskFailures) {
                askFailures_ = 0;
                ELOG("UsbLoop", "phone %04x:%04x on %s refused the accessory requests: left alone", phone.vendor,
                     phone.product, phone.port.c_str());
                leaveAlone(phone);
            }
            return false;
        }
        askFailures_ = 0;
        ELOG("UsbLoop", "phone %04x:%04x on %s asked to start Android Auto (accessory protocol %u)", phone.vendor,
             phone.product, phone.port.c_str(), protocol);
        ours_.insert(phone.port);
        // The car's own USB service opens new accessories too: be there when it comes back.
        const uint32_t begin = monoMs();
        while (!host_.stopRequested() && monoMs() - begin < kReturnMs) {
            UsbPhone back;
            AccessoryEndpoints endpoints;
            if (readUsbDevice(setup_.sysfs, phone.port, back) && back.accessory && back.address != phone.address &&
                readAccessoryEndpoints(setup_.sysfs, phone.port, endpoints)) {
                return true;
            }
            usleep(kReturnStepUs);
        }
        return false;
    }

    // The service stops: the bridge ends, and the phone is taken out of accessory mode so that it is not left
    // waiting for a head unit. Its next start then begins as a first one.
    void leave() {
        if (!bridge_) {
            return;
        }
        delete bridge_;
        bridge_ = nullptr;
        UsbPhone phone;
        if (readUsbDevice(setup_.sysfs, bridgePort_, phone) && phone.accessory) {
            UsbDevice* device = setup_.open(phone.bus, phone.address);
            const bool done = device && device->reset();
            delete device;
            ELOG("UsbLoop", "phone on %s taken out of accessory mode: %s", bridgePort_.c_str(),
                 done ? "done" : "not done, it leaves it when unplugged");
        }
    }

    const UsbLoopSetup& setup_;
    UsbHost& host_;
    UsbBridge* bridge_;
    std::string bridgePort_;
    uint32_t bridgeStart_;
    std::set<std::string> ours_;                 // ports whose accessory this loop made, or reset
    std::map<std::string, unsigned> leftAlone_;  // port and address of the devices not to touch again
    unsigned resets_, silent_, claimTries_, askFailures_;
    uint32_t quietSince_;
    bool quiet_;  // just after a reset: the bus is not looked at
    uint32_t emptySince_;
    bool empty_;  // no phone at the last look
};

}  // namespace

void runUsbLoop(const UsbLoopSetup& setup, UsbHost& host) {
    Loop(setup, host).run();
}

}  // namespace evo
