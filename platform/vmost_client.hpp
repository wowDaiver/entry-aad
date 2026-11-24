// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's vMOST client library, loaded at run time: its functions, vmostd's socket and the message layout.

// libvmostclient.so.1 is loaded with dlopen. The functions used:
//   vmostConnect(cb, user, 0)            no GLib thread: the caller runs vmostProcessMessages itself
//   vmostFblockShadowRegister(fb, inst)  / Deregister; vmostFileDescriptorGet(&fd); vmostDisconnect()
//   vmostMostMsgSend(msg)                a message laid out as VmostMsgOut
//   cb(msg, user); msg: +0 u16 address, +2 u8 FBlock, +3 u8 instance, +4 u16 function, +8 u32 OpType,
//                       +12 u16 length, +16 data pointer (the library sets it to msg+0x18).
// vmostConnect retries forever when the daemon's socket is missing: load() never opens the library without one.
// Users: the controller and volume reader (evo_knob), which lends its connection to the cluster's guidance fields
// (kombi_most), and the hotspot request (evo_wlan).
#ifndef ENTRY_AAD_PLATFORM_VMOST_CLIENT_HPP
#define ENTRY_AAD_PLATFORM_VMOST_CLIENT_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace evo {

// MOST operation types used by this program.
const uint32_t kMostOpGet = 0x1, kMostOpStartResultAck = 0x6, kMostOpStatus = 0xC;

// A received message, read from the structure the callback gets.
struct VmostMsgView {
    uint16_t addr;
    uint8_t fblock;
    uint8_t inst;
    uint16_t fkt;
    uint32_t op;
    uint16_t len;
    const uint8_t* data;
};
// Defensive read of the structure the callback receives.
bool parseVmostMsg(const void* msg, VmostMsgView& out);

// Message sent: same layout as a received one, 24 bytes on 32-bit ARM, as vmostctl builds it.
struct VmostMsgOut {
    uint16_t addr;  // 0xFFFF: address resolved by vmostd/IMC (the filter bit is set before routing)
    uint8_t fblock, inst;
    uint16_t fkt, reserved;
    uint32_t op;  // 1 = Get
    uint16_t len, reserved2;
    const uint8_t* data;
    uint32_t channel;  // 0, as vmostctl
};
// Read-only request (Get, OpType 1). data/len: its parameters (none for a plain property).
void buildVmostGet(VmostMsgOut& m, uint8_t fblock, uint8_t inst, uint16_t fkt, const uint8_t* data, uint16_t len = 0);

// The library and every function this program calls, which the compatibility check looks for (aa-supervisor --compat).
const char kVmostLibrary[] = "libvmostclient.so.1";
const char* const kVmostFunctions[] = {
    "vmostConnect",           "vmostFblockShadowRegister", "vmostFblockShadowDeregister",
    "vmostFileDescriptorGet", "vmostProcessMessages",      "vmostDisconnect",
    "vmostMostMsgSend"};
const size_t kVmostFunctionCount = sizeof(kVmostFunctions) / sizeof(kVmostFunctions[0]);

using VmostCallback = void (*)(void* msg, void* user);

// libvmostclient with its functions resolved. The library keeps static state and GLib objects: only a short session
// unloads it (evo_wlan); the controller reader keeps it until the process exits. Nothing is unloaded on destruction.
class VmostClient {
public:
    enum Load { kLoaded, kNoDaemon, kNoLibrary, kNoSymbols };
    VmostClient();
    // vmostd's socket first, then the library, then every function of kVmostFunctions. On kNoLibrary, error() holds
    // the loader's message. Anything but kLoaded leaves the library closed.
    Load load();
    const std::string& error() const { return error_; }
    void unload();

    int (*connect)(VmostCallback, void*, int);
    int (*shadowRegister)(uint8_t, uint8_t);
    int (*shadowDeregister)(uint8_t, uint8_t);
    int (*fileDescriptorGet)(int*);
    int (*processMessages)();
    int (*disconnect)();
    int (*send)(void*);

private:
    void* lib_;
    std::string error_;
};

// Host tests: a fake vmostd socket and client library instead of the car's.
void setVmostForTest(const char* socketPath, const char* library);

// The controller reader's connection, lent to another user of the bus (the cluster's guidance fields,
// platform/kombi_most.hpp): the service keeps a single vmostd connection, on the controller reader's thread.
class VmostPort {
public:
    virtual ~VmostPort() {}
    virtual int send(VmostMsgOut& m) = 0;  // 0: handed to vmostd
    virtual int shadow(uint8_t fblock, uint8_t inst, bool on) = 0;
    virtual void pump(int ms) = 0;  // reads what arrives for that long; only when the reader is closing
};

// That other user. All calls on the controller reader's thread.
class VmostPeer {
public:
    virtual ~VmostPeer() {}
    virtual bool wants(const VmostMsgView& m) const = 0;  // a message for it, not for the controller reader
    virtual void onMessage(const VmostMsgView& m) = 0;
    virtual int dueInMs(uint32_t nowMs) const = 0;  // how soon tick() has work; the reader waits 100 ms at most
    virtual void tick(uint32_t nowMs, VmostPort& port) = 0;
    virtual void finish(VmostPort& port) = 0;  // the connection closes right after
};

}  // namespace evo

#endif
