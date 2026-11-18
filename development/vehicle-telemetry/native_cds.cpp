#include "native_cds.hpp"

#include <time.h>

#include <limits>

#ifndef TELEMETRY_HOST_ABI_STUB
static_assert(sizeof(void*) == 4, "EntryNav2 adapter is ARM32 only");
static_assert(sizeof(bmw::BasicString<char>) == 4, "146 string pointer layout");
static_assert(sizeof(bmw::CdsDelegate) == 8, "146 delegate object/stub pair");
#endif
namespace telemetry {
NativeCds::Slot* NativeCds::live_[kProperties] = {};
uintptr_t NativeCds::sequence_ = 0;
NativeCds::NativeCds(bmw::PluginApp& app) : app_(app) {
    for (unsigned i = 0; i < kProperties; ++i) {
        slots_[i].index = i;
        slots_[i].reply = 0;
        slots_[i].context = 0;
        slots_[i].active = false;
        slots_[i].cookie = 0;
    }
}
NativeCds::~NativeCds() {
    for (unsigned i = 0; i < kProperties; ++i) {
        try {
            remove(i);
        } catch (...) {
        }
    }
}
bool NativeCds::available(unsigned i) {
    if (i >= kProperties) {
        return false;
    }
    bmw::BasicString<char> name(properties[i].name);
    return app_.cdsIsPropertyAvailable(name);
}
void NativeCds::subscribe(unsigned i, Reply reply, void* context) {
    if (i >= kProperties || !reply) {
        return;
    }
    Slot& slot = slots_[i];
    if (slot.active) {
        remove(i);
    }
    if (live_[i] || sequence_ == UINTPTR_MAX / 8) {
        throw 1;
    }
    slot.cookie = (++sequence_ << 3) | i;
    slot.reply = reply;
    slot.context = context;
    slot.active = true;
    live_[i] = &slot;
    bmw::BasicString<char> name(properties[i].name);
    bmw::CdsDelegate delegate = {reinterpret_cast<void*>(slot.cookie), &NativeCds::callback};
    // 146 stores the delegate before invoking its connector. Subscribe before
    // get(): cdsGetPropertyAsync alone installs its delegate after the request.
    app_.cdsAddPropertyChangedEventHandler(name, properties[i].interval_ms, delegate);
}
void NativeCds::fetch(unsigned i) {
    if (i >= kProperties || !slots_[i].active) {
        return;
    }
    bmw::BasicString<char> name(properties[i].name);
    bmw::CdsDelegate delegate = {reinterpret_cast<void*>(slots_[i].cookie), &NativeCds::callback};
    app_.cdsGetPropertyAsync(name, delegate);
}
void NativeCds::remove(unsigned i) {
    if (i >= kProperties || !slots_[i].active) {
        return;
    }
    slots_[i].active = false;
    if (live_[i] == &slots_[i]) {
        live_[i] = 0;
    }
    bmw::BasicString<char> name(properties[i].name);
    app_.cdsRemovePropertyChangedEventHandler(name);
}
bool NativeCds::callback(void* object, const bmw::BasicString<unsigned short>& value) {
    uintptr_t cookie = reinterpret_cast<uintptr_t>(object);
    unsigned i = cookie & 7;
    if (i >= kProperties || !live_[i] || live_[i]->cookie != cookie) {
        return true;
    }
    Slot& slot = *live_[i];
    if (!slot.active || !slot.reply) {
        return true;
    }
    char raw[kMaxPayload + 1];
    const unsigned short* input = value.raw();
    bool valid = input != 0;
    size_t n = 0;
    while (valid && n <= kMaxPayload) {
        unsigned short c = input[n];
        if (!c) {
            raw[n] = 0;
            break;
        }
        if (n == kMaxPayload || c > 127) {
            valid = false;
            break;
        }
        raw[n++] = static_cast<char>(c);
    }
    struct timespec ts;
    double now = std::numeric_limits<double>::quiet_NaN();
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        now = ts.tv_sec + ts.tv_nsec / 1000000000.0;
    }
    slot.reply(slot.context, slot.index, valid ? raw : 0, now);
    return true;
}
}  // namespace telemetry
