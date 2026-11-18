// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <string>

#include "controller.hpp"
#ifdef TELEMETRY_GRAPHICS
#include "dashboard/native_image.hpp"
#endif
using namespace telemetry;
static std::map<std::string, bmw::CdsDelegate> callbacks;
static std::map<unsigned, std::u16string> labels;
static unsigned subscribed = 0, removed = 0;
static bool immediate_fetch = false;
#ifdef TELEMETRY_GRAPHICS
static unsigned image_count = 0;
#endif
namespace bmw {
class IDispatcher {};
class Bundle {};
template <class T>
BasicString<T>::BasicString(const T* p) : data_(const_cast<T*>(p)) {}
template <class T>
BasicString<T>::BasicString(const BasicString& p) : data_(p.data_) {}
template <class T>
BasicString<T>::~BasicString() {}
template class BasicString<char>;
template class BasicString<unsigned short>;
PluginApp::PluginApp(IDispatcher&, BasicString<char>, unsigned, unsigned, RHMIInterfaceVersion, const Bundle&,
                     const Bundle&) {}
PluginApp::~PluginApp() {}
bool PluginApp::cdsIsPropertyAvailable(BasicString<char>&) {
    return true;
}
void PluginApp::cdsAddPropertyChangedEventHandler(BasicString<char>& p, unsigned, const CdsDelegate& d) {
    callbacks[p.raw()] = d;
    ++subscribed;
}
void PluginApp::cdsGetPropertyAsync(BasicString<char>& p, const CdsDelegate& d) {
    assert(callbacks[p.raw()].object == d.object);
    if (immediate_fetch && std::string(p.raw()) == "sensors.battery") {
        const unsigned short raw[] = {'{', '"', 'b', 'a', 't', 't', 'e', 'r', 'y', '"', ':', '8', '7', '}', 0};
        BasicString<unsigned short> value(raw);
        d.stub(d.object, value);
    }
}
void PluginApp::cdsRemovePropertyChangedEventHandler(BasicString<char>& p) {
    callbacks.erase(p.raw());
    ++removed;
}
bool PluginApp::updateModel(unsigned, const char*) {
    assert(false && "Latin-1 overload corrupts Cyrillic");
    return false;
}
bool PluginApp::updateModel(unsigned id, const BasicString<unsigned short>& s) {
    std::u16string line;
    for (const unsigned short* p = s.raw(); *p; ++p) {
        line.push_back(static_cast<char16_t>(*p));
    }
    labels[id] = line;
    return true;
}
#ifdef TELEMETRY_GRAPHICS
BufferStream::BufferStream() : fields_{} {}
BufferStream::~BufferStream() {}
Image::Image(const unsigned char* bytes, unsigned count) : path_(""), stream_(), loaded_(true) {
    assert(count > 32 && bytes[0] == 137 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G');
}
void PluginApp::updateModel(unsigned id, const Image&) {
    assert(id == 120);
    ++image_count;
}
#endif
}  // namespace bmw
double now() {
    timespec t = {};
    assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return t.tv_sec + t.tv_nsec / 1000000000.0;
}
void deliver(bmw::CdsDelegate d) {
    const unsigned short text[] = {'{', '"', 'b', 'a', 't', 't', 'e', 'r', 'y', '"', ':', '0', '}', 0};
    bmw::BasicString<unsigned short> data(text);
    d.stub(d.object, data);
}
int main() {
    bmw::IDispatcher dispatcher;
    bmw::Bundle bundle;
    bmw::PluginApp app(dispatcher, bmw::BasicString<char>("test"), 1, 0, bmw::RHMI_INTERFACE_VERSION_1, bundle, bundle);
#ifdef TELEMETRY_TEST_DEMO
    setenv("VEHICLE_TELEMETRY_DEMO", "battery-and-temperatures", 1);
    Controller demo(app, now());
    assert(demo.show(now()));
    demo.tick(now());
    assert(subscribed == 0 && callbacks.empty());
    assert(labels[105].find(u"78 % (тест)") != std::u16string::npos);
    assert(labels[109].find(u"12.6 В (тест)") != std::u16string::npos);
    assert(labels[100].find(u"92 °C (тест)") != std::u16string::npos);
    assert(labels[101].find(u"104 °C (тест)") != std::u16string::npos);
    assert(labels[114] == u"Эмулятор: тестовые данные");
    demo.hide(now());
    assert(demo.show(now()));
    assert(subscribed == 0);
    puts("emulator demo: labelled battery/temperature values; no CDS subscriptions or requests");
    return 0;
#endif
    {
        Controller c(app, now());
        assert(c.show(now()));
        assert(subscribed == 5);
        bmw::CdsDelegate old = callbacks["sensors.battery"];
        deliver(old);
        assert(c.model().reading(BatterySoc).present);
        usleep(1050000);
        c.tick(now());
        assert(labels[105].find(u"Заряд АКБ*: 0.0 %") != std::u16string::npos);
        assert(labels[100].find(u"Темп. двигателя*") == 0);
        assert(labels[114] == u"* Датчики со * требуют проверки");
        c.hide(now());
        assert(removed == 5 && callbacks.empty());
        assert(c.show(now()));
        assert(subscribed == 10);
        deliver(old);
        assert(!c.model().reading(BatterySoc).present);  // late old visit
        assert(callbacks["sensors.battery"].object != old.object);
        deliver(callbacks["sensors.battery"]);
        assert(c.model().reading(BatterySoc).present);
        c.hide(now());
        for (unsigned i = 0; i < 2000; ++i) {
            assert(c.show(now()));
            deliver(old);
            assert(!c.model().reading(BatterySoc).present);
            deliver(callbacks["sensors.battery"]);
            assert(c.model().reading(BatterySoc).present);
            c.hide(now());
        }
        assert(subscribed == 10010 && removed == 10010);
        assert(labels.size() == 26);
        assert(labels[130].find(u"Давление масла: Получение данных") == 0);
        assert(labels[131].find(u"Ток АКБ: Получение данных") == 0);
        assert(labels[132].find(u"Темп. АКБ: Получение данных") == 0);
        assert(labels[133].find(u"Передача: Получение данных") == 0);
        assert(labels[137].find(u"Выходной вал: Получение данных") == 0);
        assert(labels[106].find(u"Нет данных") != std::u16string::npos);
    }
    assert(removed == 10010);
    {
        immediate_fetch = true;
        Controller c(app, now());
        assert(c.show(now()));
        assert(c.model().reading(BatterySoc).present);
        assert(labels[105].find(u"87.0 %") != std::u16string::npos);
        assert(labels[105].find(u"Нет данных") == std::u16string::npos);
        c.hide(now());
        immediate_fetch = false;
    }
#ifdef TELEMETRY_GRAPHICS
    {
        // Physical graphics must ignore both emulator knobs and use real subscriptions.
        setenv("VEHICLE_TELEMETRY_DEMO", "battery-and-temperatures", 1);
        setenv("VEHICLE_TELEMETRY_EMULATOR", "isolated-owner146", 1);
        unsigned before = subscribed, images = image_count;
        Controller c(app, now());
        c.prime_dashboard(now());
        assert(image_count == images + 1 && subscribed == before);
        assert(c.show(now()));
        assert(subscribed == before + 5);
        c.dashboard(true, now() + 1.1);
        assert(image_count == images + 2);
        assert(!c.model().reading(EngineTemperature).present);
        assert(!c.model().reading(Voltage).present);
        assert(labels[114] == u"* Датчики со * требуют проверки");
        unsigned shown = image_count;
        c.hide(now());
        c.tick(now() + 3);
        assert(image_count == shown && callbacks.empty());
        puts(
            "physical graphics: PNG prime, native image model, real CDS, ignored demo flags and hidden teardown "
            "passed");
    }
#endif
    puts("controller: 2002 visits in fixed storage; cancelled and retired-cookie replies rejected");
}
