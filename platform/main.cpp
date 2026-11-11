// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// entry-aad service: Android Auto on the EntryNav2 head unit, wireless (from Bluetooth setup to the picture) or
// over a USB cable (--usb).

// This file reads the command line, wires the components and runs the main loop. The service's state and callbacks
// are in service_app, the picture's peeks in peek_timing, the Bluetooth half of a wireless start in wireless_loop,
// the USB half of a wired one in usb_loop.
// clang-format off
#include "alps_spp.hpp"
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
const unsigned kSppInstance = 0;  // Alps SPP instance of the head unit
const uint16_t kAaPort = 50000;   // TCP port told to the phone during the Bluetooth handshake
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
    bool ok = pid && identity && pair && instance == 0 && !strcmp(expected, pid) && reader.identity() == identity &&
              reader.read(instance, slot) && evo::snapshotMatchesBaseline(slot, pair);
    if (!ok) {
        ELOG("Main", "Alps state changed since aa-supervisor saved it: start refused");
    }
    return ok;
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
        "  --usb                  the phone comes over a USB cable: no Bluetooth, Wi-Fi or TCP server\n"
        "  --usb-port PORT        with --usb: the only USB port to watch (1-1, 3-1.2); every port otherwise\n"
        "  --no-video             no picture\n"
        "  --input forward|observe|off   controller and touchscreen to AA, only logged, or ignored\n"
        "  --service | --limit-seconds N (180 at most)\n"
        "  --alps-pid P --alps-start S --uuid-pair HEX   Alps state saved by aa-supervisor, checked before each step\n",
        argv0);
}
}  // namespace

int main(int argc, char** argv) {
    bool run = false, monitor = false, introspect = false, configure = false, video = true, service = false;
    bool usb = false;
    const char* usbPort = nullptr;
    const char* inputArg = nullptr;
    unsigned long limitSeconds = 0;
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
    const unsigned instance = kSppInstance;
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
    evo::ServiceApp app(kAaPort, video, &quit);
    app.usbPort = usbPort ? usbPort : "";
    app.session.setTouchForwarding(input == evo::INPUT_FORWARD);
    app.session.setVehicleFile(evo::contract::kVehicle);  // the car's model and year, written at installation
    app.session.setSoftwareVersion(ENTRY_AAD_VERSION);    // the file VERSION, given by the Makefile
    // Declared after app: destroyed (and joined) before the session their callbacks post to.
    evo::EvoTouch touch;
    evo::EvoKnob knob;
    evo::EvoControl control;
    if (usb) {
        ELOG("Main", "USB cable: no Bluetooth change, no Wi-Fi, no TCP server; media over the BMW Bluetooth source");
    } else {
        std::string source;
        if (!evo::loadAccessPoint(app.ap, source)) {
            ELOG("Main", "car hotspot (wlan0) or its credentials unavailable");
            return 1;
        }
        app.server.setHotspotAddress(app.ap.ip);  // listening address and the subnet a phone must come from
        if (access(evo::contract::kMaintenance, F_OK) == 0) {
            app.wifiLink.enable("wlan0");  // logs are kept: the Wi-Fi link's quality goes into them
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
    if (input != evo::INPUT_OFF) {
        app.knob = &knob;  // before the control thread, which reads it ("call" command)
    }
    // The state file is read by the iDrive app (hmiapp/).
    if (service &&
        !control.start(evo::contract::kControlPort, evo::contract::kStateFile, evo::ServiceApp::command, &app)) {
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
             "picture aside %u ms after %u ms; NAV/MEDIA/COM = AA apps, MENU/MAP/OPTION = iDrive",
             evo::kVolumePeekMs, evo::kDriveModeShowMs, evo::kDriveModeDelayMs);
        if (video && !app.pill.start(evo::kPillSurfaceId, app.videoConfig.layerId, app.videoConfig.displayWidth,
                                     app.videoConfig.displayHeight)) {
            ELOG("Main", "volume pill not started: the volume dims the picture");
        }
        knob.setVolumeCallback(evo::ServiceApp::volumeEvent, &app);
        knob.setVolumeLevelCallback(evo::ServiceApp::volumeLevel, &app);
        knob.setHardKeyCallback(evo::ServiceApp::hardKey, &app);
        knob.setAppKeyCallback(evo::ServiceApp::appKey, &app);
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
        if (service) {
            control.writeState(turn.active ? "connected" : "connecting", turn.onScreen ? 1 : 0);
        }
        if (failed) {
            break;
        }
        if (guarded && turn.idleForAdvert && advertisement.due(evo::monoMs() - startedMs)) {
            failed = !app.refreshAdvertisementIfIdle(service, alpsReader);
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
