// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// entry-aad service: Android Auto on the EntryNav2 head unit, wireless (from Bluetooth setup to the picture) or
// over a USB cable (--usb).

// This file reads the command line, wires the components and runs the main loop. The service's state and callbacks
// are in service_app, the picture's peeks in peek_timing, the Bluetooth half of a wireless start in wireless_loop,
// the USB half of a wired one in usb_loop.
// clang-format off
#include "alps_spp.hpp"
#include "car_brand.hpp"
#include "car_series.hpp"
#include "driver_side.hpp"
#include "evo_ap.hpp"
#include "evo_video.hpp"
#include "evo_touch.hpp"
#include "evo_knob.hpp"
#include "evo_control.hpp"
#include "evo_spp_snapshot.hpp"
#include "spp_baseline.hpp"
#include "spp_advertisement.hpp"
#include "evo_log.hpp"
#include "evo_pill.hpp"
#include "evo_bmw_guidance.hpp"
#include "kombi_nav.hpp"
#include "evo_gps.hpp"
#include "service_app.hpp"
#include "service_contract.hpp"
#include "aa_session.hpp"
#include "tcp_server.hpp"
#include "usb_accessory.hpp"
#include "crash_handler.hpp"
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <cerrno>
// clang-format on

namespace {
volatile sig_atomic_t quit = 0;
void onSignal(int) {
    quit = 1;
}
// This car's fixed values, observed working on the car. There are no runtime settings.
const uint16_t kAaPort = 50000;  // TCP port told to the phone during the Bluetooth handshake
const unsigned kMaxLimitSeconds = 180;

// Unsigned number from a command-line argument (decimal or 0x hex), at most max.
bool parseNumber(const char* s, unsigned long max, unsigned long& out) {
    if (!s || !*s || *s == '-') {
        return false;
    }
    char* end;
    errno = 0;
    const unsigned long v = strtoul(s, &end, 0);
    if (errno || *end || v > max) {
        return false;
    }
    out = v;
    return true;
}
// What aa-supervisor hands over on the command line: the Alps state it saved, checked again before each step.
struct AlpsBaseline {
    const char* alpsPid;
    const char* alpsStart;
    const char* uuidPair;
};
bool alpsReady(evo::SppSnapshotReader& reader, unsigned instance, const AlpsBaseline& baseline) {
    const char* pid = baseline.alpsPid;
    const char* identity = baseline.alpsStart;
    const char* pair = baseline.uuidPair;
    char expected[32];
    snprintf(expected, sizeof(expected), "%d", reader.pid());
    evo::SppSlotSnapshot slot;
    bool ok = pid && identity && pair && evo::aaSlotAllowed(instance) && !strcmp(expected, pid) &&
              reader.identity() == identity && reader.read(instance, slot) && evo::snapshotMatchesBaseline(slot, pair);
    if (!ok) {
        ELOG("Main", "Alps state changed since aa-supervisor saved it: start refused");
    }
    return ok;
}
const unsigned kBmwAppsSlot = 0;  // BMW Apps' own SPP slot, left to it while Android Auto is on slot 5
// For the [WifiLink] lines: whether a phone holds BMW Apps' link. Only reads, through the snapshot reader.
bool readAppsSlot(void* user, evo::AppsSlot& out) {
    evo::SppSlotSnapshot slot;
    if (!static_cast<evo::SppSnapshotReader*>(user)->read(kBmwAppsSlot, slot)) {
        return false;
    }
    out.connected = slot.connected;
    out.state = slot.state;
    out.transport = slot.transportState;
    return true;
}
// The Bluetooth half of a wireless start: the SPP data interface, then the Android Auto UUID, each step checked
// against the Alps state aa-supervisor saved. False when the service must stop.
bool advertiseAndroidAuto(evo::ServiceApp& app, evo::SppSnapshotReader& alpsReader, unsigned instance,
                          const AlpsBaseline& baseline, bool configure, bool guarded) {
    if (quit || !app.bt.start(instance) || !app.bt.ensureDataInterface(configure, 2000)) {
        ELOG("Main", "SPP HLI not confirmed: check 0x6203 and the instance; no UUID registered");
        return false;
    }
    ELOG("Main", "SPP interface %u confirmed: 0x6203=1", instance);
    if (quit || (guarded && !alpsReady(alpsReader, instance, baseline))) {
        return false;
    }
    int status = app.bt.registerUuid(evo::kAaUuid, 3000);
    if (status != 0) {
        ELOG("Main", "UUID registration failed (%d)", status);
        app.server.stop();
        app.session.stop();
        return false;
    }
    if (guarded) {
        evo::SppSlotSnapshot advertised;
        if (!alpsReader.read(instance, advertised) || advertised.uuidPayload != evo::androidAutoPair() ||
            advertised.transportUuidPayload != advertised.uuidPayload) {
            ELOG("Main", "Android Auto UUIDs not read back from Alps: stopping so that Bluetooth is restored");
            return false;
        }
    }
    ELOG("Main", "AA_SERVICE_READY: registration confirmed; waiting for the phone's Bluetooth link");
    ELOG("Main",
         "No Android Auto request? Turn the phone's Bluetooth off and on, then reconnect to the BMW without removing "
         "the pairing.");
    return true;
}
void usage(const char* argv0) {
    printf(
        "Usage: %s --monitor | --introspect | --run [options]\n"
        "  --monitor     watch SPP/DM; no UUID, DataReq or configuration change\n"
        "  --introspect  read the Alps API and the SPP configuration 0x6203\n"
        "  --run         register the Android Auto UUID and open the TCP server (50000)\n"
        "Options of --run, given by aa-supervisor:\n"
        "  --configure-spp        allow setting 0x6203 to 1 when needed\n"
        "  --spp-instance N       the Alps SPP slot aa-supervisor chose: 5, or 0 (default) where 5 is not started\n"
        "  --usb                  the phone comes over a USB cable: no Bluetooth, Wi-Fi or TCP server\n"
        "  --usb-port PORT        with --usb: the only USB port to watch (1-1, 3-1.2); every port otherwise\n"
        "  --no-video             no picture\n"
        "  --input forward|observe|off   controller and touchscreen to AA, only logged, or ignored\n"
        "  --service | --limit-seconds N (180 at most)\n"
        "  --alps-pid P --alps-start S --uuid-pair HEX   Alps state saved by aa-supervisor, checked before each step\n",
        argv0);
}

// The brand the iDrive app read in the car's coding (car_brand.hpp). Without the file (an older app), a BMW.
aa::CarBrand readCarBrand() {
    aa::CarBrand brand = aa::CarBrand::kBmw;
    FILE* f = fopen(evo::contract::kBrandFile, "r");
    char line[16] = {0};
    const bool read = f && fgets(line, sizeof(line), f) && aa::parseCarBrand(line, brand);
    if (f) {
        fclose(f);
    }
    if (!read) {
        ELOG("Main", "brand not read from %s: BMW", evo::contract::kBrandFile);
    }
    return brand;
}

// The driver's side the iDrive app read in the car's coding (driver_side.hpp). Without the file (an older app, or a
// coding that says nothing sure), left; the vehicle file's "side=" wins either way.
aa::DriverPosition readCodedDriverSide() {
    bool right = false;
    FILE* f = fopen(evo::contract::kDriverSideFile, "r");
    char line[16] = {0};
    const bool read = f && fgets(line, sizeof(line), f) && aa::parseDriverSide(line, right);
    if (f) {
        fclose(f);
    }
    if (read) {
        ELOG("Main", "driver side coded in the car: %s", aa::driverSideName(right));
    } else {
        ELOG("Main", "driver side not read from %s: left, unless the vehicle file says otherwise",
             evo::contract::kDriverSideFile);
    }
    return right ? aa::DriverPosition::kRight : aa::DriverPosition::kLeft;
}
// The series the iDrive app read in the car's coding (car_series.hpp), into out (6 bytes). Without the file (an older
// app, or a coding that says nothing sure), empty: the vehicle file's model, or "iDrive".
void readCarSeries(char* out) {
    out[0] = 0;
    FILE* f = fopen(evo::contract::kSeriesFile, "r");
    char line[16] = {0};
    const bool read = f && fgets(line, sizeof(line), f) && aa::parseCarSeries(line, out);
    if (f) {
        fclose(f);
    }
    if (read) {
        ELOG("Main", "series coded in the car: %s (told to the phone unless the vehicle file gives a model)", out);
    } else {
        ELOG("Main", "series not read from %s: the vehicle file's model, or iDrive", evo::contract::kSeriesFile);
    }
}
}  // namespace

int main(int argc, char** argv) {
    bool run = false, monitor = false, introspect = false, configure = false, video = true, service = false;
    bool usb = false;
    const char* usbPort = nullptr;
    const char* inputArg = nullptr;
    unsigned long limitSeconds = 0;
    unsigned long sppInstance = evo::kAaFallbackSlot;
    AlpsBaseline baseline = {nullptr, nullptr, nullptr};
    for (int i = 1; i < argc; ++i) {
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!strcmp(argv[i], "--input") && value) {
            inputArg = value;
            ++i;
        } else if (!strcmp(argv[i], "--limit-seconds") && value) {
            if (!parseNumber(value, kMaxLimitSeconds, limitSeconds) || !limitSeconds) {
                usage(argv[0]);
                return 2;
            }
            ++i;
        } else if (!strcmp(argv[i], "--spp-instance") && value) {
            if (!parseNumber(value, evo::kAaSlot, sppInstance) || !evo::aaSlotAllowed(sppInstance)) {
                usage(argv[0]);
                return 2;
            }
            ++i;
        } else if (!strcmp(argv[i], "--alps-pid") && value) {
            baseline.alpsPid = value;
            ++i;
        } else if (!strcmp(argv[i], "--alps-start") && value) {
            baseline.alpsStart = value;
            ++i;
        } else if (!strcmp(argv[i], "--uuid-pair") && value) {
            baseline.uuidPair = value;
            ++i;
        } else if (!strcmp(argv[i], "--usb-port") && value) {
            usbPort = value;
            ++i;
        } else if (!strcmp(argv[i], "--usb")) {
            usb = true;
        } else if (!strcmp(argv[i], "--service")) {
            service = true;  // started by the iDrive app through aa-supervisor: no time limit, same safety checks
        } else if (!strcmp(argv[i], "--run")) {
            run = true;
        } else if (!strcmp(argv[i], "--monitor")) {
            monitor = true;
        } else if (!strcmp(argv[i], "--introspect")) {
            introspect = true;
        } else if (!strcmp(argv[i], "--configure-spp")) {
            configure = true;
        } else if (!strcmp(argv[i], "--no-video")) {
            video = false;
        } else {
            usage(argv[0]);
            return !strcmp(argv[i], "--help") ? 0 : 2;
        }
    }
    if (static_cast<int>(run) + monitor + introspect != 1 || (!run && (configure || !video || usb)) ||
        (usb && configure) || (usbPort && (!usb || !evo::validUsbPort(usbPort)))) {
        usage(argv[0]);
        return 2;
    }
    aa::installProcessGuards();
    // A crash must not leave an Android Auto arrow on the cluster: cancelled after the crash line is logged.
    aa::setFatalHook(evo::kombi::Arrows::onFatalSignal);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGHUP, onSignal);
    const unsigned instance = static_cast<unsigned>(sppInstance);
    if (!run) {
        evo::AlpsSppLink bt;
        bt.setMonitorOnly(true);
        if (!bt.start(static_cast<uint8_t>(instance))) {
            return 1;
        }
        if (introspect) {
            std::string xml = bt.introspect("/org/alps/evolution/hli");
            printf("%s\n", xml.c_str());
            std::vector<uint8_t> cfg;
            bool ok = bt.readConfiguration(instance, 0x6203, cfg, 2000);
            if (ok && cfg.size() == 1) {
                ELOG("Main", "instance %u: spp_data_interface=%u", instance, cfg[0]);
            }
            return xml.empty() || !ok ? 1 : 0;
        }
        while (!quit && bt.running()) {
            usleep(100000);
        }
        return quit ? 0 : 1;
    }
    evo::InputMode input = evo::INPUT_OFF;
    if (!evo::parseInputMode(inputArg, input)) {
        ELOG("Main", "invalid --input (forward, observe or off)");
        return 2;
    }
    if (service && limitSeconds) {
        ELOG("Main", "--service and --limit-seconds cannot be combined");
        return 2;
    }
    const bool guarded = !usb && (limitSeconds || service);  // Alps baseline/child snapshot checks
    setenv("XDG_RUNTIME_DIR", "/run/system/0", 0);           // read by the Wayland library (picture, volume pill)
    // The panel's size, read from the compositor: the phone's canvas and the part of it that is shown follow it.
    aa::DisplayPlan display;
    if (video) {
        evo::EvoIlm ilm;
        uint32_t panelWidth = 0, panelHeight = 0;
        if (!ilm.screenSize(panelWidth, panelHeight) || !aa::planDisplay(panelWidth, panelHeight, display)) {
            ELOG("Main", "screen size unreadable or larger than 1920x1080 (%ux%u): no picture possible", panelWidth,
                 panelHeight);
            return 1;
        }
        ELOG("Main", "screen %ux%u: the phone is asked for %ux%u%s", display.width, display.height, display.canvasWidth,
             display.canvasHeight, display.reference() ? "" : " (a size not tried on the reference car)");
    }
    const aa::CarBrand brand = readCarBrand();
    display.hiddenSide = aa::surroundSide(brand, display.width);
    if (display.hiddenSide) {
        ELOG("Main",
             "%s: the round surround hides %u columns on each side, the phone keeps its UI inside (%ux%u, touches "
             "shifted by %u)",
             aa::brandName(brand), display.hiddenSide, display.phoneWidth(), display.phoneHeight(),
             display.leftInset());
    }
    evo::ServiceApp app(kAaPort, video, &quit);
    app.videoConfig.setDisplay(display.canvasWidth, display.canvasHeight, display.width, display.height);
    app.videoConfig.holdCloseFrames = !usb;  // pairs come from the Wi-Fi's bursts, not the cable
    app.session.setDisplay(display);
    app.session.setCarMake(aa::brandName(brand));
    app.session.setCodedDriverSide(readCodedDriverSide());
    static char series[6];  // kept for the session's whole life
    readCarSeries(series);
    app.session.setCodedModel(series[0] ? series : nullptr);
    app.usbPort = usbPort ? usbPort : "";
    app.session.setTouchForwarding(input == evo::INPUT_FORWARD);
    app.session.setVehicleFile(evo::contract::kVehicle);  // the car's model, year, side, fuel; written at installation
    app.session.setSoftwareVersion(ENTRY_AAD_VERSION);    // the file VERSION, given by the Makefile
    // Declared after app: destroyed (and joined) before the session their callbacks post to.
    evo::EvoTouch touch;
    touch.setPhoneDisplay(display.leftInset(), display.phoneWidth(), display.phoneHeight());
    evo::EvoKnob knob;
    evo::EvoControl control;
    const bool logsKept = access(evo::contract::kMaintenance, F_OK) == 0;
    if (usb) {
        ELOG("Main", "USB cable: no Bluetooth change, no Wi-Fi, no TCP server; media over the BMW Bluetooth source");
    } else {
        std::string source;
        if (!evo::loadAccessPoint(app.ap, source)) {
            ELOG("Main", "car hotspot (wlan0) or its credentials unavailable");
            return 1;
        }
        app.server.setHotspotAddress(app.ap.ip);  // listening address and the subnet a phone must come from
        if (logsKept) {
            app.wifiLink.enable("wlan0");  // the Wi-Fi link's quality goes into them
        }
        ELOG("Main", "access point read from %s; TCP %s:%u; media over the BMW Bluetooth source", source.c_str(),
             app.ap.ip.c_str(), static_cast<unsigned>(kAaPort));
    }
    app.server.setConnectionCallback(evo::ServiceApp::connected, &app);
    app.session.setDisconnectCallback(evo::ServiceApp::ended, &app);
    app.session.setVideoFrameCallback(evo::ServiceApp::frame, &app);
    app.session.setCodecConfigCallback(evo::ServiceApp::codec, &app);
    app.session.setVideoFocusCallback(evo::ServiceApp::videoFocus, &app);
    app.session.setMicRequestCallback(evo::ServiceApp::micRequest, &app);
    app.mic.start(evo::ServiceApp::micData, &app);  // optional: without it the Assistant just hears nothing
    app.session.setNavStatusCallback(evo::ServiceApp::navStatus, &app);
    app.kombi.attachMost(&app.kombiMost);  // F-series: the cluster's MOST fields, through the controller reader
    app.kombi.start(evo::kombi::probeBmwGuidance);  // HUD/cluster arrows; the BMW navigation always wins
    evo::CarGpsReader carGps;
    evo::CarGpsFeed gpsFeed;
    gpsFeed.logSource();
    app.session.postCarLocation(carGps.poll(aa::locationClockMs()));
    if (!usb && !app.server.start()) {
        return 1;
    }
    evo::SppSnapshotReader alpsReader;
    if (guarded && (!alpsReader.open() || !alpsReady(alpsReader, instance, baseline))) {
        return 1;
    }
    if (guarded && instance != kBmwAppsSlot) {
        app.wifiLink.setAppsProbe(readAppsSlot, &alpsReader);  // same thread as the refreshes that use the reader
    }
    if (input != evo::INPUT_OFF) {
        app.knob = &knob;  // before the control thread, which reads it ("call" command)
    }
    // The state file is read by the iDrive app (hmiapp/).
    if (service &&
        !control.start(evo::contract::kControlName, evo::contract::kStateFile, evo::ServiceApp::command, &app)) {
        return 1;
    }
    if (usb) {
        ELOG("Main", "AA_SERVICE_READY: waiting for an Android phone on %s%s", usbPort ? "USB port " : "a USB port",
             usbPort ? usbPort : "");
    } else if (!advertiseAndroidAuto(app, alpsReader, instance, baseline, configure, guarded)) {
        return 1;
    }
    if (input != evo::INPUT_OFF) {
        ELOG("Main",
             "inputs %s: touchscreen UDP 30567 and controller vMOST E5 (nothing sent to iDrive or the controller)",
             evo::inputModeName(input));
        // Optional: a failure leaves the projection running without that input.
        if (!touch.start(input, evo::ServiceApp::touchEvent, &app)) {
            ELOG("Main", "touchscreen unavailable for this run");
        }
        ELOG("Main",
             "volume: pill over AA from the first level received (before that: picture dimmed %u ms); driving mode: "
             "picture aside %u ms after %u ms; NAV/MAP/MEDIA/COM = AA apps, MENU/OPTION = iDrive",
             evo::kVolumePeekMs, evo::kDriveModeShowMs, evo::kDriveModeDelayMs);
        if (video && !app.pill.start(evo::kPillSurfaceId, app.videoConfig.layerId, app.videoConfig.displayWidth,
                                     app.videoConfig.displayHeight)) {
            ELOG("Main", "volume pill not started: the volume dims the picture");
        }
        knob.setVolumeCallback(evo::ServiceApp::volumeEvent, &app);
        knob.setVolumeLevelCallback(evo::ServiceApp::volumeLevel, &app);
        knob.setAudioButtonCallback(evo::ServiceApp::audioButton, &app);
        knob.setHardKeyCallback(evo::ServiceApp::hardKey, &app);
        knob.setAppKeyCallback(evo::ServiceApp::appKey, &app);
        knob.setMediaCallback(evo::ServiceApp::mediaKey, &app);
        {
            // The car's key coding, read by the iDrive app inside iDrive (controller_layout.hpp). Without it (an older
            // app), the G-style table.
            FILE* f = fopen(evo::contract::kControllerFile, "r");
            char line[128] = {0};
            aa::ControllerCoding coding = aa::defaultControllerCoding();
            if (f && fgets(line, sizeof(line), f) && aa::parseControllerCoding(line, coding)) {
                knob.setCoding(coding);
            } else {
                ELOG("Main", "controller coding not read from %s: G-style keys", evo::contract::kControllerFile);
            }
            if (f) {
                fclose(f);
            }
        }
        {
            FILE* f = fopen(evo::contract::kDayNightFile, "r");
            int v = 0;  // written by the iDrive app (CDS)
            if (f) {
                if (fscanf(f, "daynight %d", &v) == 1) {
                    app.applyDayNight(v, "last known state");
                }
                fclose(f);
            }
        }
        knob.setPeer(&app.kombiMost);
        if (!knob.start(input, evo::ServiceApp::knobEvent, &app)) {
            ELOG("Main", "controller unavailable for this run");
        }
    }
    app.markReady();
    pthread_t phones;  // the wireless loop, or the USB one
    if (pthread_create(&phones, nullptr, usb ? evo::ServiceApp::usb : evo::ServiceApp::wireless, &app) != 0) {
        app.server.stop();
        app.session.stop();
        return 1;
    }
    bool failed = false;
    const uint32_t startedMs = evo::monoMs();
    bool paceRunning = false;  // logs kept: the phone's longest silence and the frame gaps, every 10 s of a session
    uint32_t paceMs = 0;
    evo::AdvertisementSchedule advertisement(service ? 60000 : 0);
    while (!quit) {
        if (limitSeconds && evo::monoMs() - startedMs >= limitSeconds * 1000) {
            break;
        }
        app.session.postCarLocation(gpsFeed.next(carGps));
        const evo::ServiceApp::Turn turn = app.turn(service);
        failed = turn.failed;
        // Inputs reach the phone only while its picture covers the screen; otherwise they stay with iDrive.
        const bool owned = input == evo::INPUT_FORWARD && turn.shown && turn.pictureUp;
        touch.setPictureStarted(turn.pictureStarted);
        touch.setOwned(owned);
        knob.setOwned(owned);
        if (turn.focusMsg && turn.active) {
            app.session.postVideoFocus(turn.focusMsg == 1);
        }
        app.wifiLink.turn(evo::monoMs(), turn.active);
        if (logsKept) {
            // Wireless or USB: how long the phone went without sending anything. It pings and reports its media
            // every second while it projects, so several seconds here are a link that stalled, whatever the cause.
            // Then the video frames that came right behind the previous one, and how many were held (Wi-Fi only):
            // whether this car gets them in pairs (FrameGap).
            const uint32_t nowMs = evo::monoMs();
            if (!turn.active) {
                paceRunning = false;
            } else if (!paceRunning) {
                paceRunning = true;
                paceMs = nowMs;
                app.session.takeLongestSilenceMs();  // the start of the session is not a silence
                app.video.takeFrameGaps();
            } else if (nowMs - paceMs >= 10000) {
                const evo::FrameGapStats gaps = app.video.takeFrameGaps();
                ELOG("Session",
                     "in %u s: the phone's longest silence %u ms; video: %u frames, %u within %u ms of the previous "
                     "one, %u held (longest %u ms)",
                     (nowMs - paceMs + 500) / 1000, app.session.takeLongestSilenceMs(), gaps.frames, gaps.early,
                     evo::kFrameGapMs, gaps.held, gaps.longestHoldMs);
                paceMs = nowMs;
            }
        }
        if (service) {
            // 1: on screen; 2: connected and aside, waiting for the iDrive app's "show" (hmiapp/service_state.hpp).
            control.writeState(turn.active ? "connected" : "connecting", turn.onScreen ? 1 : (turn.asks ? 2 : 0));
        }
        if (failed) {
            break;
        }
        if (guarded && turn.idleForAdvert && advertisement.due(evo::monoMs() - startedMs)) {
            failed = !app.refreshAdvertisementIfIdle(service, alpsReader, instance);
            if (failed) {
                break;
            }
        }
        usleep(100000);
    }
    app.requestStop();
    app.bt.interruptLink();
    pthread_join(phones, nullptr);
    touch.setOwned(false);
    knob.setOwned(false);
    touch.stop();
    knob.stop();        // UDP 30567 back to iDrive, vMOST copies deregistered, before the session goes
    app.pill.stop();    // nothing calls show() any more
    control.stop();     // state file removed: the iDrive app shows "Android Auto" again
    app.server.stop();  // join the accept thread before stopping its last session
    app.session.stop();
    app.video.stop();
    app.bt.stop();
    if (input != evo::INPUT_OFF) {
        touch.summary();
        knob.summary();
    }
    bool firstFrame = app.video.firstFrameSeen();
    ELOG("Main",
         "summary: spp_links=%u wpp_reply=%u wifi_accepted=%u tcp_connections=%u usb_connections=%u "
         "h264_frames=%u compositor_frame_callback=%u",
         app.sppLinks, app.wppReply ? 1 : 0, app.wppAccepted ? 1 : 0, app.tcpLinks, app.usbLinks,
         app.videoFrames.load(), firstFrame ? 1 : 0);
    if (!firstFrame && limitSeconds) {
        ELOG("Main", "summary: NO FRAME CALLBACK FROM THE COMPOSITOR; see the steps above");
    }
    return failed ? 1 : (limitSeconds && video && !firstFrame ? 2 : 0);
}
