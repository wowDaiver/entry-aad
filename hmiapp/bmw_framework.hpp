// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Stand-ins for BMW's RHMI app framework (libAppFramework.A.so, libCoreFramework.A.so), and the few tricks the app
// needs on top of it: its own bmw::Runnable, a bmw::PluginApp whose vtable calls the app's onInit/onError/onFinalize.
//
// No BMW header exists here: only names and parameter types matter, they give the mangled symbols the car's libraries
// export (tests/test_hmiapp_load.cpp loads the app against them). Facts read on the copy of the car's system:
//   - bmw::PluginApp's primary vtable has 34 slots: onError 21, onInit 22, onFinalize 23 (a secondary
//     PluginAppHandler vtable follows); its constructor writes up to +0x174, within kPluginAppStorage.
//   - bmw::Runnable has no data member: its destructor only resets the vtable pointer (libCoreFramework). The
//     framework keeps the Runnable's address in a Message and calls run() through that pointer.
#ifndef ENTRY_AAD_HMIAPP_BMW_FRAMEWORK_HPP
#define ENTRY_AAD_HMIAPP_BMW_FRAMEWORK_HPP

#include <stddef.h>

namespace bmw {
class IDispatcher;
class Bundle;
class Runnable;
class Parcel {
public:
    explicit Parcel(bool value);       // _ZN3bmw6ParcelC1Eb
    ~Parcel();                         // _ZN3bmw6ParcelD1Ev
    void readBool(bool& value) const;  // _ZNK3bmw6Parcel8readBoolERb
private:
    unsigned char m_storage[32];
};
enum RHMIInterfaceVersion { RHMI_INTERFACE_VERSION_1 = 1 };
template <typename T>
class BasicString {
public:
    BasicString(const T* s);                // _ZN3bmw11BasicStringIcEC1EPKc
    BasicString(const BasicString& other);  // _ZN3bmw11BasicStringIcEC1ERKS1_
    ~BasicString();                         // _ZN3bmw11BasicStringIcED1Ev
    const T* raw() const { return m_data; }

private:
    T* m_data;
};
class TextID {
public:
    explicit TextID(unsigned id);  // _ZN3bmw6TextIDC1Ej
private:
    unsigned m_id, m_type;
};
class ImageID {
public:
    explicit ImageID(unsigned id);  // _ZN3bmw7ImageIDC1Ej
private:
    unsigned m_id, m_type;
};
template <typename R, typename A1, typename A2, typename A3, typename A4>
struct Delegate {
    void* object;
    R (*stub)(void* object, A1 a1);
};
using ActionDelegate = Delegate<bool, const Parcel&, void, void, void>;
using CdsDelegate = Delegate<bool, const BasicString<unsigned short>&, void, void, void>;
const size_t kPluginAppStorage = 0x200;  // EntryEVO constructor writes up to +0x174
class PluginApp {
public:
    PluginApp(IDispatcher& dispatcher, BasicString<char> name, unsigned major, unsigned minor,
              RHMIInterfaceVersion rhmi, const Bundle& params, const Bundle& config);
    void addHandler(unsigned actionId, const ActionDelegate& handler);
    void addHandler(unsigned id, unsigned type, const ActionDelegate& handler);
    void triggerEvent(unsigned eventId);
    void triggerEvent(unsigned eventId, const Parcel& value);  // _ZN3bmw9PluginApp12triggerEventEjRKNS_6ParcelE
    void post(Runnable* runnable, unsigned delayMs);
    void updateModel(unsigned modelId, const TextID& text);
    void updateModel(unsigned modelId, const ImageID& image);
    void updateModel(unsigned modelId, bool value);
    void setProperty(unsigned componentId, unsigned propertyId, const Parcel& value);
    void cdsAddPropertyChangedEventHandler(BasicString<char>& property, unsigned id, const CdsDelegate& handler);
    bool onInit();
    void onFinalize();

private:
    unsigned char m_storage[kPluginAppStorage];
};
}  // namespace bmw

namespace evo {
namespace app {

// ---- tasks on the app thread ---------------------------------------------------------------------------------------
// A bmw::Runnable of our own: BMW's vtable (offset, typeinfo, D1, D0, run) copied, destructors made no-ops, run() ours.
struct AppRunnable {
    void** vptr;  // where a bmw::Runnable keeps its vtable pointer, and all it holds
    void* vtbl[5];
};
void makeRunnable(AppRunnable& r, void (*run)(void*));
void postRunnable(bmw::PluginApp* app, AppRunnable& r, unsigned delayMs);

// ---- handlers ------------------------------------------------------------------------------------------------------
typedef bool (*ActionStub)(void* app, const bmw::Parcel& value);
typedef bool (*CdsStub)(void* app, const bmw::BasicString<unsigned short>& value);
void addAction(bmw::PluginApp* app, unsigned actionId, ActionStub stub);
void addStateHandler(bmw::PluginApp* app, unsigned state, unsigned kind, ActionStub stub);
void addCdsHandler(bmw::PluginApp* app, const char* property, CdsStub stub);

bool parcelBool(const bmw::Parcel& value);
// A CDS text (UTF-16) as ASCII, '?' for anything else, cut to cap - 1 characters.
void narrow(const bmw::BasicString<unsigned short>& value, char* text, size_t cap);

// ---- the app object ------------------------------------------------------------------------------------------------
struct PluginAppHooks {
    bool (*onInit)(bmw::PluginApp* app);
    void (*onError)(bmw::PluginApp* app, unsigned code, const bmw::BasicString<char>& text);
    void (*onFinalize)(bmw::PluginApp* app);
};
// A bmw::PluginApp in storage of its own, given a copy of its primary vtable with the three hooks in place.
// *bmwVtable: the vtable BMW's constructor set, for the log.
bmw::PluginApp* newPluginApp(bmw::IDispatcher& dispatcher, const char* name, const bmw::Bundle& params,
                             const bmw::Bundle& config, const PluginAppHooks& hooks, void** bmwVtable);
// Its deleting destructor (vtable slot 1).
void deletePluginApp(void* app);

}  // namespace app
}  // namespace evo

#endif
