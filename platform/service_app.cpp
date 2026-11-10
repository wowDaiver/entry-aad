// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The entry-aad service's state and callbacks (see service_app.hpp).
#include "service_app.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "evo_log.hpp"
#include "evo_spp_snapshot.hpp"
#include "service_contract.hpp"
#include "spp_advertisement.hpp"

namespace evo {

ServiceApp::ServiceApp(uint16_t p, bool v, volatile sig_atomic_t* quit)
    : knob(nullptr),
      server(p),
      port(p),
      videoEnabled(v),
      stopping(false),
      wirelessDone(false),
      focus(true),
      ready(false),
      tcpPreparing(false),
      reverse(false),
      pendingFocus(0),
      sppLinks(0),
      tcpLinks(0),
      usbLinks(0),
      videoFrames(0),
      wppReply(false),
      wppAccepted(false),
      quit_(quit) {
    pthread_mutex_init(&mu, nullptr);
    pthread_mutex_init(&connectMu, nullptr);
    pthread_mutex_init(&videoMu, nullptr);
}

ServiceApp::~ServiceApp() {
    pill.stop();
    server.stop();
    mic.stop();
    kombi.stop();
    session.stop();
    video.stop();
    bt.stop();
    pthread_mutex_destroy(&mu);
    pthread_mutex_destroy(&connectMu);
    pthread_mutex_destroy(&videoMu);
}

void ServiceApp::markReady() {
    pthread_mutex_lock(&mu);
    ready = true;
    pthread_mutex_unlock(&mu);
}

void ServiceApp::requestStop() {
    pthread_mutex_lock(&mu);
    stopping = true;
    pthread_mutex_unlock(&mu);
}

bool ServiceApp::stopRequested() {
    pthread_mutex_lock(&mu);
    bool v = stopping;
    pthread_mutex_unlock(&mu);
    return v;
}

bool ServiceApp::projectionBusy() {
    pthread_mutex_lock(&mu);
    bool v = tcpPreparing;
    pthread_mutex_unlock(&mu);
    return v || session.isActive();
}

void ServiceApp::sppLinkOpened() {
    pthread_mutex_lock(&mu);
    ++sppLinks;
    pthread_mutex_unlock(&mu);
}

void ServiceApp::handshakeEnded(bool spoke, bool accepted) {
    pthread_mutex_lock(&mu);
    wppReply = wppReply || spoke;
    wppAccepted = wppAccepted || accepted;
    pthread_mutex_unlock(&mu);
}

ServiceApp::Turn ServiceApp::turn(bool service) {
    Turn t;
    pthread_mutex_lock(&mu);
    t.failed = wirelessDone;
    t.active = session.isActive();
    t.onScreen = videoEnabled && t.active && focus && !reverse;  // what the iDrive app is told
    const uint32_t nowMs = monoMs();
    const PeekTiming::Picture picture = peek.turn(nowMs, t.onScreen);
    t.shown = picture.shown;
    if (!t.shown) {
        pill.hide();  // camera, menu, popup: the pill never stays over iDrive
    }
    t.focusMsg = pendingFocus;
    pendingFocus = 0;
    t.idleForAdvert = !tcpPreparing && !t.active && (service || (!sppLinks && !tcpLinks));
    pthread_mutex_unlock(&mu);
    t.pictureUp = false;
    t.pictureStarted = false;
    if (videoEnabled) {
        pthread_mutex_lock(&videoMu);
        video.setVisible(t.shown);
        const bool opacityRefused = t.shown && !video.setOpacity(picture.opacity) && picture.opacity < 1.f;
        t.pictureUp = video.isVisible();
        t.pictureStarted = video.isReady();
        pthread_mutex_unlock(&videoMu);
        if (opacityRefused) {
            ELOG("ServiceApp", "the compositor refused the opacity: the volume now steps the picture aside");
            pthread_mutex_lock(&mu);
            peek.refuseOpacity(monoMs(), nowMs);
            pthread_mutex_unlock(&mu);
        }
    }
    return t;
}

bool ServiceApp::refreshAdvertisementIfIdle(bool service, SppSnapshotReader& reader) {
    // Same lock order as connected(): a TCP session cannot start during an idle refresh.
    bool failed = false;
    pthread_mutex_lock(&connectMu);
    pthread_mutex_lock(&mu);
    bool idle = !tcpPreparing && !session.isActive() && (service || (!sppLinks && !tcpLinks));
    pthread_mutex_unlock(&mu);
    if (idle) {
        failed = !refreshAdvertisement(bt, reader);
    }
    pthread_mutex_unlock(&connectMu);
    return !failed;
}

void ServiceApp::frame(uint32_t, const uint8_t* data, size_t len, uint64_t pts, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    if (a->videoEnabled) {
        // Phone frames never wait for mu: the main loop holds it while LayerManagerControl runs (show/hide,
        // opacity), which would stall the picture.
        ++a->videoFrames;
        a->video.postVideoFrame(data, len, pts);
    }
}

void ServiceApp::codec(const uint8_t* data, size_t len, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    if (a->videoEnabled) {
        a->video.postCodecConfig(data, len);
    }
}

void ServiceApp::touchEvent(const aa::TouchEventOut& ev, void* user) {
    static_cast<ServiceApp*>(user)->session.postTouchEvent(ev);
}

// Volume key (knob or steering wheel). With the pill working, the level message that follows shows it.
void ServiceApp::volumeEvent(unsigned steps, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->mu);
    const bool start = a->peek.volumeKey(monoMs(), a->pill.ready());
    const bool refused = a->peek.dimRefused();
    pthread_mutex_unlock(&a->mu);
    if (start) {
        ELOG("ServiceApp", "volume (%u step(s)): %s %u ms", steps, refused ? "AA picture aside" : "AA picture dimmed",
             kVolumePeekMs);
    }
}

// Volume level from the car (vMOST thread): the pill over Android Auto, which stays opaque.
void ServiceApp::volumeLevel(const VolumeLevel& level, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->mu);
    const bool first = !a->peek.levelSeen();
    const bool ready = a->pill.ready();
    if (a->peek.volumeLevel(ready)) {
        a->pill.show(level.value, level.min, level.max);  // only takes the pill's own short lock
    }
    pthread_mutex_unlock(&a->mu);
    if (first) {
        ELOG("ServiceApp", "volume level received from the car: pill %s", ready ? "on" : "unavailable");
    }
}

// Hard key iDrive acts on (MENU, MEDIA, NAV, MAP, COM; not forwarded to the phone): the picture steps aside at
// once, so the result does not depend on timing (observed on the car otherwise: iDrive sometimes came back,
// sometimes not). If iDrive stays on the blank Android Auto page the iDrive app opens its main menu. The Android
// Auto row brings the phone back.
void ServiceApp::hardKey(const char* name, void* user) {
    // OPTION opens the Android Auto menu of the iDrive app (Stop, start automatically): see the app's hold logic.
    // NAV and MEDIA always go to Android Auto (appKey); COM reaches here only during a call.
    static const char* const leave[] = {"MENU", "MAP", "COM", "OPTIONS"};
    bool match = false;
    for (unsigned i = 0; i < sizeof(leave) / sizeof(leave[0]); ++i) {
        if (!strcmp(name, leave[i])) {
            match = true;
        }
    }
    if (!match) {
        return;
    }
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->mu);
    const bool was = a->peek.hardKey();
    if (was) {
        a->focus = false;
        a->pendingFocus = 2;
    }
    pthread_mutex_unlock(&a->mu);
    if (was) {
        ELOG("ServiceApp",
             "key %s: AA picture aside, iDrive takes the screen back (the Android Auto row brings it back)", name);
    }
}

void ServiceApp::knobEvent(const aa::CcsEvent& ev, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    if (ev.kind == 1) {
        a->session.postKeyEvent(ev.keycode, ev.down != 0);
    } else if (ev.kind == 2) {
        a->session.postRelativeEvent(ev.keycode, ev.delta);
    }
}

// NAV / MEDIA / COM / steering wheel phone opened an Android Auto app (evo_knob.cpp): Android Auto stays on screen.
// iDrive also switched screens underneath; the iDrive app sees this file and puts iDrive back on its blank state.
void ServiceApp::appKey(const char* name, void* user) {
    (void)user;
    const std::string tmp = std::string(contract::kAppKeyFile) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (f) {
        fprintf(f, "appkey %s\n", name);
        fclose(f);
        rename(tmp.c_str(), contract::kAppKeyFile);
    }
    ELOG("ServiceApp", "key %s: opened in Android Auto (as with CarPlay), iDrive brought back under Android Auto",
         name);
}

// Commands from the iDrive app (service mode): show, hide, drivemode, popup, gear, call, daynight, quit.
void ServiceApp::command(const ControlCommand& c, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->mu);
    if (c.verb == "show") {
        a->focus = true;
        a->pendingFocus = 1;
    } else if (c.verb == "hide") {
        a->focus = false;
        a->pendingFocus = 2;
    } else if (c.verb == "drivemode") {
        // Sent by the iDrive app only for a real change between two valid modes.
        if (a->peek.driveMode(monoMs())) {
            ELOG("ServiceApp", "driving mode %d: AA picture aside %u ms (after %u ms) for iDrive's display",
                 c.hasArg ? c.arg : -1, kDriveModeShowMs, kDriveModeDelayMs);
        }
    } else if (c.verb == "popup") {
        const PeekTiming::Popup popup = a->peek.popup(monoMs(), c.arg != 0);
        if (popup == PeekTiming::kPopupShown) {
            ELOG("ServiceApp", "iDrive popup: AA picture aside %u ms at most", kPopupShowMs);
        } else if (popup == PeekTiming::kPopupGone) {
            ELOG("ServiceApp", "iDrive popup closed: AA picture back");
        }
    } else if (c.verb == "gear") {
        const bool r = c.arg == 2;  // CDS driving.gear reverse (2 = reverse), logged by the app
        if (r != a->reverse) {
            ELOG("ServiceApp", "reverse gear %s: AA picture %s", r ? "engaged" : "left", r ? "hidden" : "back");
        }
        a->reverse = r;
    } else if (c.verb == "call") {  // iDrive app (CDS communication.callStatus): phone keys go to iDrive's call screen
        if (a->knob) {
            a->knob->setPhoneToIdrive(c.arg != 0);
        }
        ELOG("ServiceApp", "call %s: phone keys %s", c.arg ? "in progress" : "ended",
             c.arg ? "left to iDrive" : "to Android Auto");
    }
    pthread_mutex_unlock(&a->mu);
    if (c.verb == "daynight") {
        a->applyDayNight(c.arg, "iDrive app");
    }
    if (c.verb == "quit") {
        *a->quit_ = 1;
    }
}

void ServiceApp::applyDayNight(int v, const char* src) {
    if (v != 1 && v != 2) {
        return;
    }
    session.postNightMode(v == 2);
    ELOG("ServiceApp", "car day/night = %s (%s): Android Auto theme %s", v == 2 ? "night" : "day", src,
         v == 2 ? "dark" : "light");
}

void ServiceApp::videoFocus(bool projected, uint32_t reason, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->mu);
    a->focus = projected;
    pthread_mutex_unlock(&a->mu);
    ELOG("ServiceApp", "video focus=%s reason=%u", projected ? "AA" : "BMW", reason);
}

// Phone opened/closed the car mic (Assistant). Session thread: non-blocking.
void ServiceApp::micRequest(bool open, void* user) {
    static_cast<ServiceApp*>(user)->mic.request(open);
    ELOG("ServiceApp", "phone: microphone %s", open ? "open" : "closed");
}

void ServiceApp::micData(const uint8_t* pcm, size_t len, void* user) {
    static_cast<ServiceApp*>(user)->session.postMicData(pcm, len);
}

// AA navigation status (ch 10): next maneuver for the cluster/HUD. Session thread: non-blocking.
void ServiceApp::navStatus(uint16_t msgId, const uint8_t* body, size_t len, void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    evo::kombi::Arrows::onNavStatus(msgId, body, len, &a->kombi);
}

void ServiceApp::ended(void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    // No stop()/join here: this callback runs on the AA session thread.
    a->mic.request(false);
    a->kombi.onSessionEnded();
    pthread_mutex_lock(&a->mu);
    a->focus = false;
    pthread_mutex_unlock(&a->mu);
    a->server.notifySessionEnded();
    ELOG("ServiceApp", "session ended");
}

void ServiceApp::connected(int fd, void* user) {
    connectedTransport(aa::socketTransport(fd), user);
}

void ServiceApp::connectedTransport(aa::Transport* transport, void* user) {
    if (!transport) {
        return;
    }
    ServiceApp* a = static_cast<ServiceApp*>(user);
    pthread_mutex_lock(&a->connectMu);
    if (a->stopRequested()) {
        delete transport;
        pthread_mutex_unlock(&a->connectMu);
        return;
    }
    pthread_mutex_lock(&a->mu);
    bool ready = a->ready;
    pthread_mutex_unlock(&a->mu);
    if (!ready) {
        delete transport;
        a->server.notifySessionEnded();
        pthread_mutex_unlock(&a->connectMu);
        return;
    }
    pthread_mutex_lock(&a->mu);
    if (!strcmp(transport->name(), "USB/AOA")) {
        ++a->usbLinks;
    } else {
        ++a->tcpLinks;
    }
    a->tcpPreparing = true;
    pthread_mutex_unlock(&a->mu);
    ELOG("ServiceApp", "phone transport connection received; preparing the video and the AA session");
    a->session.stop();  // join callbacks before destroying/reinitializing video buffers
    bool videoOk = true;
    if (a->videoEnabled) {
        pthread_mutex_lock(&a->videoMu);
        a->video.stop();
        videoOk = a->video.init(a->videoConfig) && a->video.start();
        pthread_mutex_unlock(&a->videoMu);
    }
    pthread_mutex_lock(&a->mu);
    if (!videoOk) {
        delete transport;
        a->server.notifySessionEnded();
        a->tcpPreparing = false;
        pthread_mutex_unlock(&a->mu);
        pthread_mutex_unlock(&a->connectMu);
        return;
    }
    a->focus = true;
    pthread_mutex_unlock(&a->mu);
    if (!a->session.start(transport)) {
        a->server.notifySessionEnded();  // start owns/cleans transport even on failure
    } else {
        a->server.markSessionActive();
        if (!a->session.isActive()) {
            a->server.notifySessionEnded();
        }
    }
    pthread_mutex_lock(&a->mu);
    a->tcpPreparing = false;
    pthread_mutex_unlock(&a->mu);
    pthread_mutex_unlock(&a->connectMu);
}

void* ServiceApp::wireless(void* user) {
    ServiceApp* a = static_cast<ServiceApp*>(user);
    runWirelessLoop(a->bt, a->ap, a->port, *a);
    pthread_mutex_lock(&a->mu);
    a->wirelessDone = true;
    pthread_mutex_unlock(&a->mu);
    return nullptr;
}

}  // namespace evo
