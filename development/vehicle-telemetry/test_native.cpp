#include <assert.h>
#include <math.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "native_cds.hpp"
using namespace telemetry;
static bmw::CdsDelegate current;
static std::vector<std::string> calls;
namespace bmw {
template <class T>
BasicString<T>::BasicString(const T* s) : data_(const_cast<T*>(s)) {}
template <class T>
BasicString<T>::BasicString(const BasicString& s) : data_(s.data_) {}
template <class T>
BasicString<T>::~BasicString() {}
template class BasicString<char>;
template class BasicString<unsigned short>;
bool PluginApp::cdsIsPropertyAvailable(BasicString<char>& s) {
    calls.push_back(s.raw());
    return true;
}
void PluginApp::cdsAddPropertyChangedEventHandler(BasicString<char>& s, unsigned interval, const CdsDelegate& d) {
    assert(interval == properties[4].interval_ms);
    assert(std::string(s.raw()) == "sensors.battery");
    current = d;
    calls.push_back("subscribe");
}
void PluginApp::cdsGetPropertyAsync(BasicString<char>&, const CdsDelegate& d) {
    assert(current.object == d.object);
    calls.push_back("get");
}
void PluginApp::cdsRemovePropertyChangedEventHandler(BasicString<char>&) {
    calls.push_back("remove");
}
}  // namespace bmw
static unsigned received = 0, invalid = 0;
static std::string last;
void receive(void*, unsigned i, const char* raw, double now) {
    assert(i == 4 && isfinite(now));
    ++received;
    if (raw) {
        last = raw;
    } else {
        ++invalid;
    }
}
void deliver(const unsigned short* p) {
    bmw::BasicString<unsigned short> value(p);
    current.stub(current.object, value);
}
int main() {
    // Test-only storage: this test exercises the transport ABI boundary, not
    // the real PluginApp constructor or firmware lifecycle.
    alignas(8) unsigned char memory[sizeof(bmw::PluginApp)] = {};
    bmw::PluginApp& app = *reinterpret_cast<bmw::PluginApp*>(memory);
    NativeCds t(app);
    assert(!t.available(99));
    assert(t.available(4));
    t.subscribe(4, receive, 0);
    t.fetch(4);
    assert(calls[1] == "subscribe" && calls[2] == "get");
    const unsigned short good[] = {'{', '"', 'b', 'a', 't', 't', 'e', 'r', 'y', '"', ':', '0', '}', 0};
    deliver(good);
    assert(last == "{\"battery\":0}" && received == 1);
    std::vector<unsigned short> exact(1025, 'x');
    exact[1024] = 0;
    deliver(exact.data());
    assert(last.size() == 1024 && invalid == 0);
    std::vector<unsigned short> large(1026, 'x');
    large[1025] = 0;
    deliver(large.data());
    assert(invalid == 1);
    const unsigned short unicode[] = {0x410, 0};
    deliver(unicode);
    deliver(0);
    assert(invalid == 3);
    t.remove(4);
    t.remove(4);
    deliver(good);
    assert(received == 5);
    assert(calls.back() == "remove" && calls.size() == 4);
    bmw::CdsDelegate retired = current;
    unsigned before = received;
    for (unsigned visit = 0; visit < 10000; ++visit) {
        t.subscribe(4, receive, 0);
        t.fetch(4);
        retired.stub(retired.object, bmw::BasicString<unsigned short>(good));
        assert(received == before);
        deliver(good);
        ++before;
        assert(received == before);
        t.remove(4);
    }
    {
        NativeCds temporary(app);
        temporary.subscribe(4, receive, 0);
        retired = current;
    }
    // A delegate may survive destruction of its owner; no pointer into the
    // freed object is dereferenced, including after another owner is installed.
    retired.stub(retired.object, bmw::BasicString<unsigned short>(good));
    assert(received == before);
    t.subscribe(4, receive, 0);
    retired.stub(retired.object, bmw::BasicString<unsigned short>(good));
    assert(received == before);
    deliver(good);
    assert(received == before + 1);
    t.remove(4);
    puts("native boundary: property/interval, UTF-16 bounds, invalid input and late replies passed");
}
