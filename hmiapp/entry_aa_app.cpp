// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// libEntryAad.so: Android Auto as a native iDrive "PluginApp" on EntryNav2 / EntryEVO (Linux), the framework of
// BMW's SecurityMenu / ActiveSeating. This file is the app object: its creation by HmiMain3D, the entry button and the
// menu, the focus of its states handed to the hold machine, and the status poll that keeps them in step with the
// service.
//
// EntryEVO facts (reverse-engineered on the copied rootfs):
//   - CHmiGenMain::start adds getDataPath(0)+"/Plugins" (-ro=/opt/hmi/data/ro) and getDataPath(1)+"/Plugins"
//     (-rw=/var/local/data/HMI). The writable one is mounted noexec (PerspartitionMount.sh: /var/local noexec,nosuid,
//     nodev; confirmed on the car), so dlopen fails there. The installation therefore adds
//     /opt/hmi/data/ro/Plugins/EntryAad and /opt/aa on the system partition (new folders only);
//     settings and logs stay in /var/local/data/AA (data only, nothing executed from there).
//   - ManagedPluginApp::init: {pluginDir}/EntryAad -> libEntryAad.so; dlsym "pluginAppInfo"; create at +0x10,
//     destroy at +0x14.
//   - CSpecialAppsHelper::getPluginAppConnector has no coding gate on this HU.
//   - ST_Ent_Selection is native state 20 here too (CRHMIEntertainmentInfo::onEntryPointEvent compares with 0x14).
// The other parts of the app:
//   bmw_framework  stand-ins for BMW's framework: declarations, the app's Runnable, the PluginApp vtable hooks
//   app_log        app.log, maintenance profile only
//   app_guards     off switch, crash guard, start automatically and its boot-loop guard
//   service_link   the supervisor /opt/aa/aa-supervisor, control messages, the service's state, the stop sequence
//   car_signals    gear, phone calls, day/night and driving mode from the car's CDS
//   hold_machine   iDrive kept on the blank state while Android Auto is on screen (pure, tested on the PC)
//   pending_restart  the restart the USB checkbox asks for while Android Auto runs (pure, tested on the PC)
// Uninstall: delete the plugin folder and reboot.
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../platform/service_contract.hpp"
#include "app_guards.hpp"
#include "app_log.hpp"
#include "bmw_framework.hpp"
#include "car_signals.hpp"
#include "hold_machine.hpp"
#include "mono_clock.hpp"
#include "pending_restart.hpp"
#include "rhmi_ids.hpp"
#include "service_link.hpp"
#include "service_state.hpp"

namespace evo {
namespace app {

namespace {

const unsigned kStatusPollMs = 1000;
const unsigned kSpinMs = 200;

bmw::PluginApp* g_app = nullptr;     // created by HmiMain3D, until destroyed
bmw::PluginApp* g_runApp = nullptr;  // initialized, until finalized: the posted tasks run while it is set
time_t g_initTime = 0;

// ---- the hold (hmiapp/hold_machine.hpp): iDrive's focus in, iDrive events and control messages out ------------------
class AppHoldIo : public HoldIo {
public:
    void triggerEvent(unsigned event) override {
        if (g_app) {
            g_app->triggerEvent(event);
        }
    }
    void send(const char* message) override { sendToService(message); }
    void log(const char* line) override { alog("%s", line); }
    ServiceState readService() override { return readServiceState(); }
    // The one delay read from a date, the file's: 2 s, between the service writing it and this app seeing it.
    bool appKeyAge(long* seconds) override {
        struct stat st;
        if (stat(contract::kAppKeyFile, &st) != 0) {
            return false;
        }
        *seconds = static_cast<long>(time(nullptr) - st.st_mtime);
        return true;
    }
    void removeAppKey() override { unlink(contract::kAppKeyFile); }
};
AppHoldIo g_holdIo;
HoldMachine g_hold(g_holdIo);

// iDrive's focus on the blank state, our menu or its Help page, handed to the hold machine.
bool onAaFocus(void*, const bmw::Parcel& p) {
    g_hold.aaFocus(parcelBool(p));
    return true;
}
bool onAaVisible(void*, const bmw::Parcel& p) {
    g_hold.aaVisible(parcelBool(p), monoSeconds());
    return true;
}
bool menuFocus(unsigned state, const bmw::Parcel& p) {
    const bool focused = parcelBool(p);
    g_hold.menuFocus(state, focused, monoSeconds(), monoMs());
    return true;
}
bool onMenuFocus(void*, const bmw::Parcel& p) {
    return menuFocus(kMenuState, p);
}
bool onHelpFocus(void*, const bmw::Parcel& p) {
    return menuFocus(kHelpState, p);
}

// ---- connecting animation of the entry button -----------------------------------------------------------------------
AppRunnable g_spinRunnable;
bool g_spinWanted = false, g_spinRunning = false;
unsigned g_spinFrame = 0;

void spinRun(void*) {
    bmw::PluginApp* app = g_runApp;
    if (!app) {
        g_spinRunning = false;
        return;
    }
    if (!g_spinWanted) {
        bmw::ImageID icon(kImgEntryIcon);
        app->updateModel(kEntryImageModel, icon);
        g_spinRunning = false;
        return;
    }
    bmw::ImageID frame(kImgSpinFirst + (g_spinFrame++ % kSpinFrames));
    app->updateModel(kEntryImageModel, frame);
    postRunnable(app, g_spinRunnable, kSpinMs);
}

void setConnectingAnimation(bmw::PluginApp* app, bool on) {
    if (on == g_spinWanted) {
        return;
    }
    g_spinWanted = on;
    bmw::Parcel busy(on);
    app->setProperty(kStartButton, kPropWaiting, busy);
    if (on && !g_spinRunning) {
        makeRunnable(g_spinRunnable, &spinRun);
        g_spinRunning = true;
        postRunnable(app, g_spinRunnable, kSpinMs);
    }
}

// ---- the entry button and the menu ----------------------------------------------------------------------------------
bool g_settingsShown = false;
void showSettings(bmw::PluginApp* app) {
    app->updateModel(kAutostartModel, autostartOn());
    app->updateModel(kUsbCableModel, usbCableOn());
    g_settingsShown = true;
}
PendingRestart g_restart;  // the USB checkbox changed while Android Auto ran

bool guardedByOverlay(const char* what) {
    ServiceState d = readServiceState();
    if (d.pid > 0 && d.overlay) {
        alog("%s ignored: Android Auto is on screen (stray knob input)", what);
        return true;
    }
    return false;
}

// Android Auto connected and nothing against showing it (stop in progress, call, reverse gear).
bool readyToShow(const ServiceState& d) {
    return d.pid > 0 && !strcmp(d.state, "connected") && !stopInProgress() && !callActive() &&
           lastGear() != kReverseGear;
}

void startServiceOrShow() {
    ServiceState d = readServiceState();
    if (d.pid > 0) {
        alog("start: service running (pid %d, %s): asking it to come to the front", d.pid, d.state);
        g_hold.requestShow();  // the coming overlay is new: hold iDrive on state 13
    } else if (d.pid < 0) {
        alog("start: service starting (%s): waiting for it", d.state);
    } else {
        g_restart.cancel();
        launchService();
    }
}

bool onStartRow(void*, const bmw::Parcel&) {
    alog("menu: Start (raAction %u)", kActStart);
    if (stopInProgress()) {
        alog("start ignored: a stop is in progress");
        return true;
    }
    forgetBootGuard();
    // Connected: the row reads "Open Android Auto". Same path as the direct entry button (entry-direct), validated on
    // the car.
    if (readyToShow(readServiceState())) {
        g_hold.showAndHold("Open row", monoSeconds());
        return true;
    }
    startServiceOrShow();
    return true;
}

bool onStopRow(void*, const bmw::Parcel&) {
    alog("menu: Stop (raAction %u)", kActStop);
    g_restart.cancel();  // stopped by hand: it stays stopped
    if (stopInProgress() || guardedByOverlay("stop")) {
        return true;
    }
    beginStop();
    return true;
}

// Entry button "Android Auto" (iDrive Media menu): the app's menu, so Stop never requires Android Auto on screen;
// its first row opens Android Auto. Android Auto starts if it is not running (it comes up by itself once connected).
// With the entry-direct file: as CarPlay's, connected -> Android Auto on screen at once.
bool onEntry(void* self, const bmw::Parcel&) {
    bmw::PluginApp* app = static_cast<bmw::PluginApp*>(self);
    ServiceState d = readServiceState();
    alog("entry: Android Auto selected (service %s, pid %d)", d.state, d.pid);
    if (access(contract::kEntryDirect, F_OK) == 0 && readyToShow(d)) {
        g_hold.showAndHold("entry button", monoSeconds());
        return true;
    }
    app->triggerEvent(kEvOpenMenu);
    if (d.pid == 0 && !stopInProgress()) {
        forgetBootGuard();
        startServiceOrShow();
    }
    return true;
}

bool onAutostartToggle(void*, const bmw::Parcel&) {
    if (guardedByOverlay("start automatically")) {
        if (g_runApp) {
            showSettings(g_runApp);  // the checkbox flipped on screen: show the unchanged setting again
        }
        return true;
    }
    toggleAutostart();
    if (g_runApp) {
        showSettings(g_runApp);
    }
    return true;
}

// Wireless or USB cable: chosen here and nowhere else, never guessed from what is plugged in (a passenger's
// phone on charge must not take Android Auto over). The service reads the choice at its start: one that runs is
// stopped, and started again by the status poll once nothing of it is left.
bool onUsbCableToggle(void*, const bmw::Parcel&) {
    if (!guardedByOverlay("USB cable")) {
        toggleUsbCable();
        if (stopInProgress()) {
            if (g_restart.asked()) {
                g_restart.ask(monoSeconds());  // changed again during its own restart
            }
        } else if (readServiceState().pid != 0) {
            alog("menu: link changed while Android Auto runs: stopping it, then starting it again");
            beginStop();
            g_restart.ask(monoSeconds());
        }
    }
    if (g_runApp) {
        showSettings(g_runApp);  // the setting as it is now, changed or not
    }
    return true;
}

// ---- the status poll: service state -> entry text, menu rows, hold, stop --------------------------------------------
AppRunnable g_statusRunnable;
unsigned g_shownText = kTextDefault;
unsigned g_shownRow = kTextStart;

void statusRun(void*) {
    bmw::PluginApp* app = g_runApp;
    if (!app) {
        return;
    }
    if (!g_settingsShown) {
        showSettings(app);
    }
    resetGuardsWhenStable(g_initTime);
    serviceStop();
    if (g_restart.asked()) {
        const bool busy = stopInProgress() || readServiceState().pid != 0 || supervisorRunning();
        const PendingRestart::Turn restart = g_restart.turn(monoSeconds(), busy);
        if (restart == PendingRestart::kStart) {
            alog("link changed: Android Auto has stopped, starting it again over %s",
                 usbCableOn() ? "the USB cable" : "wireless");
            forgetBootGuard();
            launchService();
        } else if (restart == PendingRestart::kGiveUp) {
            alog("link changed: Android Auto still there after %ld s, not started again (Start in the menu)",
                 kRestartGiveUpS);
        }
    }
    serviceAutostart(g_initTime);
    ServiceState d = readServiceState();
    bool connecting = !strcmp(d.state, "connecting"), connected = !strcmp(d.state, "connected");
    unsigned want = connecting ? kTextConnecting : kTextDefault;
    if (want != g_shownText) {
        bmw::TextID text(want);
        app->updateModel(kEntryTextModel, text);
        alog("entry text -> %u (%s)", want, d.state);
        g_shownText = want;
    }
    setConnectingAnimation(app, connecting);
    // Menu: "Stop Android Auto" only while it runs; the call state is (re)sent when the phone connects.
    static int s_stopShown = -1;
    const int stopWanted = d.pid != 0 ? 1 : 0;
    if (stopWanted != s_stopShown) {
        bmw::Parcel visible(stopWanted != 0);
        app->setProperty(kStopButton, kPropVisible, visible);
        s_stopShown = stopWanted;
    }
    // A (re)started service knows neither: started in reverse, Android Auto would come up over the camera.
    g_hold.tellService(d);
    unsigned row = connected ? kTextRowOpen : connecting ? kTextRowConnecting : kTextStart;
    if (row != g_shownRow) {
        bmw::TextID text(row);
        app->updateModel(kStartRowModel, text);
        alog("start row text -> %u (%s)", row, d.state);
        g_shownRow = row;
    }
    g_hold.poll(d, monoSeconds());
    postRunnable(app, g_statusRunnable, kStatusPollMs);
}

// ---- the car's CDS properties, subscribed once the app runs ---------------------------------------------------------
AppRunnable g_subRunnable;
void subscribeRun(void*) {
    if (!g_runApp) {
        return;
    }
    subscribeCarSignals(g_runApp, g_hold);
}

// ---- the app object -------------------------------------------------------------------------------------------------
bool appOnInit(bmw::PluginApp* self) {
    alog("onInit: begin");
    addAction(self, kActStart, &onStartRow);
    addAction(self, kActStop, &onStopRow);
    addAction(self, kActEntry, &onEntry);
    addAction(self, kActAutostart, &onAutostartToggle);
    addAction(self, kActUsbCable, &onUsbCableToggle);
    addStateHandler(self, kMenuState, kHandlerFocus, &onMenuFocus);
    addStateHandler(self, kHelpState, kHandlerFocus, &onHelpFocus);
    addStateHandler(self, kAaState, kHandlerFocus, &onAaFocus);
    addStateHandler(self, kAaState, kHandlerVisible, &onAaVisible);
    g_initTime = time(nullptr);
    bool ok = self->onInit();
    alog("onInit: handlers registered, base onInit -> %d, start automatically %s, USB cable %s", static_cast<int>(ok),
         autostartOn() ? "on" : "off", usbCableOn() ? "on" : "off");
    g_runApp = self;
    makeRunnable(g_subRunnable, &subscribeRun);
    postRunnable(self, g_subRunnable, 1500);
    makeRunnable(g_statusRunnable, &statusRun);
    postRunnable(self, g_statusRunnable, 2000);
    return ok;
}

void appOnError(bmw::PluginApp*, unsigned code, const bmw::BasicString<char>& text) {
    const char* s = text.raw();
    alog("onError %u: %s", code, s ? s : "(null)");
}

void appOnFinalize(bmw::PluginApp* self) {
    alog("onFinalize");
    g_runApp = nullptr;
    self->onFinalize();
}

bmw::PluginApp* createApp(bmw::IDispatcher& dispatcher, const char* name, const bmw::Bundle& params,
                          const bmw::Bundle& config) {
    mkdir(contract::kDataDir, 0755);
    alog("create(name=%s) %s, HMI pid %d", name ? name : "(null)", ENTRY_AA_APP_VERSION, static_cast<int>(getpid()));
    if (appDisabled() || !crashGuardAllows()) {
        return nullptr;
    }
    if (g_app) {
        alog("create: already created");
        return nullptr;
    }
    const PluginAppHooks hooks = {&appOnInit, &appOnError, &appOnFinalize};
    void* base = nullptr;
    bmw::PluginApp* app = newPluginApp(dispatcher, name, params, config, hooks, &base);
    g_app = app;
    alog("create: app %p (PluginApp vtable %p)", static_cast<void*>(app), base);
    return app;
}

void destroyApp(void* app) {
    alog("destroy(%p)", app);
    if (!app) {
        return;
    }
    deletePluginApp(app);
    if (app == g_app) {
        g_app = nullptr;
    }
}

__attribute__((constructor)) void libraryLoaded() {
    mkdir(contract::kDataDir, 0755);
    Dl_info info;
    memset(&info, 0, sizeof(info));
    const char* self =
        (dladdr(reinterpret_cast<void*>(&libraryLoaded), &info) && info.dli_fname) ? info.dli_fname : "?";
    alog("library loaded into pid %d from %s, %s; supervisor %s %s", static_cast<int>(getpid()), self,
         ENTRY_AA_APP_VERSION, contract::kSupervisor,
         access(contract::kSupervisor, X_OK) == 0 ? "executable" : "MISSING or not executable");
}

}  // namespace
}  // namespace app
}  // namespace evo

// What ManagedPluginApp::init reads with dlsym: create at +0x10, destroy at +0x14. The library's only exported symbol.
extern "C" {
struct EntryPluginAppInfo {
    uint16_t versionMajor;
    uint16_t versionMinor;
    const char* name;
    const char* vendor;
    uint32_t rhmiInterfaceVersion;
    bmw::PluginApp* (*create)(bmw::IDispatcher&, const char*, const bmw::Bundle&, const bmw::Bundle&);
    void (*destroy)(void*);
};
__attribute__((visibility("default"))) EntryPluginAppInfo pluginAppInfo = {
    1, 0, "Android Auto", "EntryAad", 1, &evo::app::createApp, &evo::app::destroyApp};
}
