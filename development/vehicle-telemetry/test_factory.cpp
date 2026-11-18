// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "native_cds.hpp"
bool sameBuild(const char*, const unsigned char*, unsigned);
static unsigned calls = 0;
namespace bmw {
class IDispatcher {};
class Bundle {};
template <class T>
BasicString<T>::BasicString(const T* p) : data_(const_cast<T*>(p)) {
    ++calls;
}
template <class T>
BasicString<T>::BasicString(const BasicString& p) : data_(p.data_) {
    ++calls;
}
template <class T>
BasicString<T>::~BasicString() {
    ++calls;
}
template class BasicString<char>;
template class BasicString<unsigned short>;
PluginApp::PluginApp(IDispatcher&, BasicString<char>, unsigned, unsigned, RHMIInterfaceVersion, const Bundle&,
                     const Bundle&) {
    ++calls;
}
PluginApp::~PluginApp() {
    ++calls;
}
void PluginApp::addHandler(unsigned, unsigned, const ActionDelegate&) {
    ++calls;
}
bool PluginApp::onInit() {
    ++calls;
    return true;
}
void PluginApp::onFinalize() {
    ++calls;
}
void PluginApp::post(Runnable*, unsigned) {
    ++calls;
}
bool PluginApp::updateModel(unsigned, const char*) {
    ++calls;
    return true;
}
bool PluginApp::updateModel(unsigned, const BasicString<unsigned short>&) {
    ++calls;
    return true;
}
bool PluginApp::cdsIsPropertyAvailable(BasicString<char>&) {
    ++calls;
    return true;
}
void PluginApp::cdsAddPropertyChangedEventHandler(BasicString<char>&, unsigned, const CdsDelegate&) {
    ++calls;
}
void PluginApp::cdsGetPropertyAsync(BasicString<char>&, const CdsDelegate&) {
    ++calls;
}
void PluginApp::cdsRemovePropertyChangedEventHandler(BasicString<char>&) {
    ++calls;
}
void Parcel::readBool(bool& b) const {
    ++calls;
    b = false;
}
}  // namespace bmw
extern "C" {
struct PluginAppInfo {
    uint16_t major, minor;
    const char* name;
    const char* vendor;
    uint32_t rhmi;
    bmw::PluginApp* (*create)(bmw::IDispatcher&, const char*, const bmw::Bundle&, const bmw::Bundle&);
    void (*destroy)(void*);
};
extern PluginAppInfo pluginAppInfo;
}
int main(int argc, char** argv) {
    assert(argc == 4 && strlen(argv[3]) == 40);
    unsigned char id[20];
    for (unsigned i = 0; i < 20; ++i) {
        unsigned byte = 0;
        assert(sscanf(argv[3] + 2 * i, "%2x", &byte) == 1);
        id[i] = static_cast<unsigned char>(byte);
    }
    assert(sameBuild(argv[1], id, 20));
    assert(!sameBuild(argv[2], id, 20));
    assert(!sameBuild("/no-such-firmware-file", id, 20));
    bmw::IDispatcher dispatcher;
    bmw::Bundle bundle;
    assert(!pluginAppInfo.create(dispatcher, "test", bundle, bundle));
    assert(calls == 0);
    pluginAppInfo.destroy(0);
    assert(calls == 0);
    puts("factory: donor gate accepts original146, rejects156; foreign host rejected before BMW calls");
}
