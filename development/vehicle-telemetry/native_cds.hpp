#pragma once
#include <stdint.h>

#include "probe.hpp"

// Independently declared from the archived 146 exports/disassembly. This is a
// read-only adapter for an ALREADY INITIALIZED PluginApp, not a plugin factory.
namespace bmw {
class IDispatcher;
class Bundle;
enum RHMIInterfaceVersion { RHMI_INTERFACE_VERSION_1 = 1 };
template <class T>
class BasicString {
public:
    explicit BasicString(const T*);
    BasicString(const BasicString&);
    ~BasicString();
    const T* raw() const { return data_; }

private:
    T* data_;
};
template <class R, class A, class B, class C, class D>
struct Delegate {
    void* object;
    R (*stub)(void*, A);
};
typedef Delegate<bool, const BasicString<unsigned short>&, void, void, void> CdsDelegate;
class Image;
class Parcel {
public:
    void readBool(bool&) const;
};
typedef Delegate<bool, const Parcel&, void, void, void> ActionDelegate;
class Runnable {
public:
    virtual ~Runnable() {}
    virtual void run() = 0;
};
class alignas(8) PluginApp {
public:
    PluginApp(IDispatcher&, BasicString<char>, unsigned, unsigned, RHMIInterfaceVersion, const Bundle&, const Bundle&);
    ~PluginApp();
    void addHandler(unsigned, unsigned, const ActionDelegate&);
    bool onInit();
    void onFinalize();
    void post(Runnable*, unsigned);
    bool updateModel(unsigned, const char*);
    void updateModel(unsigned, const Image&);
    bool updateModel(unsigned, const BasicString<unsigned short>&);
    bool cdsIsPropertyAvailable(BasicString<char>&);
    void cdsAddPropertyChangedEventHandler(BasicString<char>&, unsigned, const CdsDelegate&);
    void cdsGetPropertyAsync(BasicString<char>&, const CdsDelegate&);
    void cdsRemovePropertyChangedEventHandler(BasicString<char>&);

private:
    // Native 146 allocation/ctor extent is 0x178. Oversized storage keeps our
    // state separate; it does not claim an exact reverse-engineered sizeof.
    unsigned char storage_[0x200] __attribute__((unused));
};
}  // namespace bmw
namespace telemetry {
class NativeCds : public Transport {
public:
    explicit NativeCds(bmw::PluginApp& app);
    ~NativeCds();
    bool available(unsigned);
    void subscribe(unsigned, Reply, void*);
    void fetch(unsigned);
    void remove(unsigned);

private:
    struct Slot {
        unsigned index;
        Reply reply;
        void* context;
        bool active;
        uintptr_t cookie;
    };
    // Dispatcher-thread registry. Opaque generation cookies never dereference a
    // retired context, so visits can reuse fixed storage without retaining it.
    static Slot* live_[kProperties];
    static uintptr_t sequence_;
    static bool callback(void*, const bmw::BasicString<unsigned short>&);
    bmw::PluginApp& app_;
    Slot slots_[kProperties];
    NativeCds(const NativeCds&);
    NativeCds& operator=(const NativeCds&);
};
}  // namespace telemetry
