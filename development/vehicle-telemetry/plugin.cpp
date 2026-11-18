// SPDX-License-Identifier: GPL-3.0-or-later
// PluginApp lifecycle/vtable pattern follows kizapp's GPL-3.0-or-later
// src/native_app.cpp at 5d7b6da; full provenance in vendor/kizapp/SOURCE.json.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <new>

#include "controller.hpp"
#include "menu_visibility.hpp"
#include "persistent_guard.hpp"
#include "profile146.hpp"
#include "trial_guard.hpp"
#include "vendor/kizapp/profile_gate.hpp"

namespace {
#ifdef TELEMETRY_EMULATOR_ONLY
#define VM_LOG(message) fprintf(stderr, "VEHICLE_TELEMETRY_VM %s\n", message)
#else
#define VM_LOG(message) ((void)0)
#endif
#ifdef TELEMETRY_GRAPHICS
const unsigned TickMilliseconds = 50;
#else
const unsigned TickMilliseconds = 200;
#endif
struct State {
    bmw::PluginApp* app;
    telemetry::Controller* controller;
    void** native_vtable;
    void* vtable[36];
    bool initialized, shown, tick_pending;
    bool exit_pending;
    bool finalizing;
    telemetry::MenuVisibility visibility;
    telemetry::PersistentGuard guard;
} state = {};
double now() {
    timespec ts = {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        return -1;
    }
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}
class Tick : public bmw::Runnable {
    void run() {
        state.tick_pending = false;
        if (!state.initialized || !state.shown || !state.app || !state.controller) {
            return;
        }
        if (!state.visibility.active(now())) {
            state.shown = false;
            state.controller->hide(now());
            return;
        }
        try {
            state.controller->tick(now());
            state.app->post(this, TickMilliseconds);
            state.tick_pending = true;
        } catch (...) {
            state.guard.fail("tick-exception");
            state.initialized = state.shown = false;
            state.controller->hide(now());
        }
    }
} tick;
class HealthTick : public bmw::Runnable {
    void run() {
        if (!state.initialized || !state.app) {
            return;
        }
#if !defined(TELEMETRY_EMULATOR_ONLY) && !defined(TELEMETRY_HOST_ABI_STUB)
        if (state.guard.healthy()) {
            telemetry::trial_log("persistent-start-healthy");
        } else {
            state.guard.fail("health-record-failed");
            state.initialized = state.shown = false;
            if (state.controller) {
                state.controller->hide(now());
            }
        }
#endif
    }
} health_tick;
class ExitTick : public bmw::Runnable {
    void run() {
        state.exit_pending = false;
        try {
            if (state.initialized && state.controller && !state.visibility.active(now())) {
                state.shown = false;
                state.controller->hide(now());
            } else if (state.initialized && !state.visibility.visible()) {
                state.app->post(this, 100);
                state.exit_pending = true;
            }
        } catch (...) {
            state.visibility.clear();
            state.shown = false;
            if (state.controller) {
                state.controller->hide(now());
            }
        }
    }
} exit_tick;
bool known_abi() {
    bool hmi = sameBuild("/proc/self/exe", Hmi146, sizeof(Hmi146));
#ifdef TELEMETRY_EMULATOR_ONLY
    // In the lab HMI is started by an explicit modern ld-linux for VirGL.
    // Check the mapped HMI ELF itself, retaining the exact146 Build ID gate.
    // This fallback is absent from the physical-HU candidate.
    if (!hmi) {
        const char* enabled = getenv("VEHICLE_TELEMETRY_EMULATOR");
        Dl_info info = {};
        void* anchor = dlsym(RTLD_DEFAULT, "_ZN15CHmiDescription4loadEP15IHmiPropertyBag");
        hmi = enabled && !strcmp(enabled, "isolated-owner146") && anchor && dladdr(anchor, &info) && info.dli_fname &&
              sameBuild(info.dli_fname, Hmi146, sizeof(Hmi146));
        if (hmi) {
            VM_LOG("exact146 mapped HMI accepted under explicit lab loader");
        }
    }
#endif
    if (!hmi) {
        VM_LOG("HMI Build ID rejected");
        return false;
    }
    // Runnable's weak vtable is also emitted by HMI and AppFramework. Strong,
    // unique API exports identify their actual providers without ELF preemption.
    const char* anchors[] = {"_ZN3bmw9PluginApp6onInitEv", "_ZNK3bmw6Parcel8readBoolERb"};
    const char* modules[] = {"libAppFramework.A.so", "libCoreFramework.A.so"};
    const unsigned char* ids[] = {App146, Core146};
    for (unsigned i = 0; i < 2; ++i) {
        // HMI also exposes canonical PLT addresses for imported functions.
        // Query each already-loaded DSO's own scope, without loading a new one.
        void* module = dlopen(modules[i], RTLD_LAZY | RTLD_NOLOAD);
        void* address = module ? dlsym(module, anchors[i]) : 0;
        Dl_info info = {};
        bool match = address && dladdr(address, &info) && info.dli_fname && sameBuild(info.dli_fname, ids[i], 20);
#ifdef TELEMETRY_EMULATOR_ONLY
        fprintf(stderr, "VEHICLE_TELEMETRY_VM anchor=%s provider=%s match=%u\n", anchors[i],
                info.dli_fname ? info.dli_fname : "none", match);
#endif
        if (module) {
            dlclose(module);
        }
        if (!match) {
            return false;
        }
    }
    return true;
}
bool visible_page(unsigned page, const bmw::Parcel& value) {
    if (!state.initialized || !state.controller) {
        return true;
    }
    bool shown = false;
    try {
        value.readBool(shown);
#ifdef TELEMETRY_EMULATOR_ONLY
        fprintf(stderr, "VEHICLE_TELEMETRY_VM state=%u visible=%u\n", page, shown);
#endif
        if (!state.visibility.change(page, shown, now())) {
            return true;
        }
#ifdef TELEMETRY_GRAPHICS
        if (page == 15) {
            state.controller->dashboard(shown, now());
        }
#endif
        if (state.visibility.active(now())) {
            state.shown = state.controller->show(now());
            if (state.shown && !state.tick_pending) {
                state.app->post(&tick, TickMilliseconds);
                state.tick_pending = true;
            }
        }
        if (!shown && !state.exit_pending) {
            state.app->post(&exit_tick, 100);
            state.exit_pending = true;
        }
    } catch (...) {
        state.guard.fail("visibility-exception");
        state.visibility.clear();
        state.shown = false;
        state.controller->hide(now());
    }
    return true;
}
bool visible10(void*, const bmw::Parcel& p) {
    return visible_page(10, p);
}
bool visible16(void*, const bmw::Parcel& p) {
    return visible_page(16, p);
}
bool visible17(void*, const bmw::Parcel& p) {
    return visible_page(17, p);
}
bool visible11(void*, const bmw::Parcel& p) {
    return visible_page(11, p);
}
bool visible12(void*, const bmw::Parcel& p) {
    return visible_page(12, p);
}
bool visible13(void*, const bmw::Parcel& p) {
    return visible_page(13, p);
}
bool visible14(void*, const bmw::Parcel& p) {
    return visible_page(14, p);
}
#ifdef TELEMETRY_GRAPHICS
bool visible15(void*, const bmw::Parcel& p) {
    return visible_page(15, p);
}
#endif
bool initialize(bmw::PluginApp* app) {
    VM_LOG("initialize entered");
    telemetry::trial_log("init-enter");
    try {
        bool (*handlers[])(void*, const bmw::Parcel&) = {visible10, visible11, visible12, visible13, visible14};
        for (unsigned i = 0; i < 5; ++i) {
            bmw::ActionDelegate handler = {app, handlers[i]};
            app->addHandler(10 + i, 11, handler);
        }
        bmw::ActionDelegate transmission = {app, visible16};
        app->addHandler(16, 11, transmission);
        bmw::ActionDelegate shafts = {app, visible17};
        app->addHandler(17, 11, shafts);
#ifdef TELEMETRY_GRAPHICS
        bmw::ActionDelegate graphic = {app, visible15};
        app->addHandler(15, 11, graphic);
#endif
        state.initialized = app->onInit();
#if !defined(TELEMETRY_EMULATOR_ONLY) && !defined(TELEMETRY_HOST_ABI_STUB)
        if (!state.guard.active()) {
            state.initialized = false;
        }
#endif
        VM_LOG(state.initialized ? "native onInit passed" : "native onInit failed");
        telemetry::trial_log(state.initialized ? "init-ok" : "init-failed");
#ifdef TELEMETRY_GRAPHICS
        // Seed the typed image model once after native initialization. This is
        // a placeholder, with no CDS/EGS requests and no background animation.
        if (state.initialized) {
            state.controller->prime_dashboard(now());
        }
#endif
        if (state.initialized) {
            app->post(&health_tick, 60000);
        } else {
            state.guard.fail("init-failed");
        }
        return state.initialized;
    } catch (...) {
        state.guard.fail("init-exception");
        telemetry::trial_log("init-exception");
        return false;
    }
}
void finalize(bmw::PluginApp* app) {
    state.finalizing = true;
    state.visibility.clear();
    state.initialized = state.shown = false;
    if (state.controller) {
        state.controller->hide(now());
    }
    try {
        app->onFinalize();
    } catch (...) {
        state.guard.fail("finalize-exception");
    }  // native finalization owns its queue
}
void error(bmw::PluginApp*, unsigned code, const bmw::BasicString<char>& message) {
#ifdef TELEMETRY_EMULATOR_ONLY
    fprintf(stderr, "VEHICLE_TELEMETRY_VM native error=%u message=%s\n", code, message.raw());
#else
    (void)code;
    (void)message;
#endif
    if (!state.finalizing) {
        state.guard.fail("native-error");
    }
    state.visibility.clear();
    state.initialized = state.shown = false;
    if (state.controller) {
        state.controller->hide(now());
    }
}
bmw::PluginApp* create(bmw::IDispatcher& dispatcher, const char* name, const bmw::Bundle& params,
                       const bmw::Bundle& config) {
    VM_LOG("factory entered");
    if (state.app || !known_abi()) {
        return 0;
    }
    state.finalizing = false;
#if !defined(TELEMETRY_EMULATOR_ONLY) && !defined(TELEMETRY_HOST_ABI_STUB)
    char boot[37];
    if (!telemetry::PersistentGuard::boot_id(boot) ||
        !state.guard.start(telemetry::TrialDirectory, boot, static_cast<unsigned long>(getpid()))) {
        telemetry::trial_log("fallback-persistent", 999, state.guard.reason());
        return 0;
    }
#endif
    telemetry::trial_log("factory-enter-persistent");
    bmw::PluginApp* app = 0;
    try {
        app = new bmw::PluginApp(dispatcher, bmw::BasicString<char>(name ? name : "VehicleTelemetry"), 1, 0,
                                 bmw::RHMI_INTERFACE_VERSION_1, params, config);
        void** base = *reinterpret_cast<void***>(app);
        void** expected = static_cast<void**>(dlsym(RTLD_DEFAULT, "_ZTVN3bmw9PluginAppE"));
        bool matches = expected && base == expected + 2;
        for (unsigned i = 0; i < 34 && matches; ++i) {
            matches = base[i] == dlsym(RTLD_DEFAULT, ExpectedSlots[i]);
        }
        if (!matches) {
            state.guard.fail("vtable-rejected");
            VM_LOG("native vtable rejected");
            delete app;
            return 0;
        }
        state.native_vtable = base;
        state.controller = new telemetry::Controller(*app, now());
        memcpy(state.vtable, base - 2, sizeof(state.vtable));
        state.vtable[23] = reinterpret_cast<void*>(error);
        state.vtable[24] = reinterpret_cast<void*>(initialize);
        state.vtable[25] = reinterpret_cast<void*>(finalize);
        *reinterpret_cast<void***>(app) = state.vtable + 2;
        state.app = app;
        VM_LOG("factory passed");
        return app;
    } catch (...) {
        state.guard.fail("factory-exception");
        delete state.controller;
        state.controller = 0;
        if (app) {
            delete app;
        }
        return 0;
    }
}
void destroy(void* pointer) {
    if (!pointer || pointer != state.app) {
        return;
    }
    state.initialized = state.shown = false;
    state.visibility.clear();
    if (state.controller) {
        state.controller->hide(now());
    }
    bmw::PluginApp* app = state.app;
    *reinterpret_cast<void***>(app) = state.native_vtable;
    // Keep all callback contexts alive through native queue/map destruction.
    try {
        delete app;
    } catch (...) {
        state.guard.fail("destroy-exception");
    }
    state.app = 0;
    state.native_vtable = 0;
    delete state.controller;
    state.controller = 0;
    state.tick_pending = state.exit_pending = false;
    state.guard.clean();
    state.guard.release();
}
}  // namespace
extern "C" {
struct PluginAppInfo {
    uint16_t major, minor;
    const char* name;
    const char* vendor;
    uint32_t rhmi;
    bmw::PluginApp* (*create)(bmw::IDispatcher&, const char*, const bmw::Bundle&, const bmw::Bundle&);
    void (*destroy)(void*);
};
#ifndef TELEMETRY_HOST_ABI_STUB
static_assert(sizeof(PluginAppInfo) == 24, "ARM32 plugin factory layout");
#endif
__attribute__((visibility("default"))) PluginAppInfo pluginAppInfo = {
    1, 0, "Параметры автомобиля", "VehicleTelemetry", 1, create, destroy};
}
