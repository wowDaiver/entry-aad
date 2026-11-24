// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto supervisor, started by the iDrive app (hmiapp/) with --service: prepares Bluetooth and Wi-Fi, runs
// entry-aad, gives Bluetooth back on exit. With the USB setting file, Bluetooth and Wi-Fi are left as they are.
// --compat rehearses a start for the compatibility check, --recover finishes a restore left unconfirmed. Never restarts
// BMW services, never edits firmware. The iDrive menu also runs it to copy the logs onto a USB stick (--copy-logs) and
// to update Android Auto from one (--update-check, --update-install: platform/update_install.hpp).
#include <arpa/inet.h>
#include <fcntl.h>
#include <libgen.h>
#include <net/if.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "alps_spp.hpp"
#include "display_plan.hpp"
#include "evo_ap.hpp"
#include "evo_elf.hpp"
#include "evo_gps.hpp"
#include "evo_ilm.hpp"
#include "evo_log.hpp"
#include "evo_spp_snapshot.hpp"
#include "evo_wlan.hpp"
#include "kept_log.hpp"
#include "log_copy.hpp"
#include "service_contract.hpp"
#include "spp_baseline.hpp"
#include "update_install.hpp"
#include "usb_accessory.hpp"
#include "vmost_client.hpp"

extern char** environ;
namespace {
volatile sig_atomic_t stopRequested = 0;
// --service: when the BMW hotspot is off, ask wland to switch it on like iDrive's Apple CarPlay switch does
// (evo_wlan.hpp). At most twice per service run, 60 s apart.
bool g_wlanAuto = false;
void maybeRequestHotspot() {
    static int attempts = 0;
    static uint32_t last = 0;
    if (!g_wlanAuto || attempts >= 2 || (attempts && evo::monoMs() - last < 60000)) {
        return;
    }
    ++attempts;
    last = evo::monoMs();
    ELOG("Supervisor", "BMW hotspot off: asking wland to switch it on, as the Apple CarPlay switch does (attempt %d/2)",
         attempts);
    evo::WlanRequestResult r;
    evo::requestWlanAccessPoint(4000, &r);
}
void stopSignal(int) {
    stopRequested = 1;
}
const uint32_t surface = 0x7eaa0001, layer = 0x80000000;
const char* journal = "bluetooth-retour.txt";
struct Baseline {
    int pid;
    std::string identity;
    unsigned instance, interface;
    std::vector<uint8_t> uuid;
};
std::vector<uint8_t> aaPayload() {
    uint8_t u[16];
    evo::parseUuid128(evo::kAaUuid, u);
    return evo::buildSetUuidPayload(u, u);
}
std::string hex(const std::vector<uint8_t>& bytes) {
    return evo::uuidPairHex(bytes);
}
// The saved state starts with its format name, kept as first written: a state saved by one version must still be
// restored by the next.
bool load(Baseline& b) {
    struct stat st;
    if (lstat(journal, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_size > 512) {
        return false;
    }
    std::ifstream f(journal);
    std::string magic, encoded, extra;
    if (!(f >> magic >> b.pid >> b.identity >> b.instance >> b.interface >> encoded) || magic != "ENTRY_AA_TRIAL_V1" ||
        b.pid <= 0 || !evo::aaSlotAllowed(b.instance) || b.interface > 1 || b.identity.empty() ||
        b.identity.find_first_not_of("0123456789") != std::string::npos || encoded.size() < 12 || encoded.size() > 68 ||
        encoded.size() % 2 || (f >> extra)) {
        return false;
    }
    b.uuid.clear();
    for (size_t i = 0; i < encoded.size(); i += 2) {
        unsigned value;
        char tail;
        if (sscanf(encoded.substr(i, 2).c_str(), "%2x%c", &value, &tail) != 1) {
            return false;
        }
        b.uuid.push_back(static_cast<uint8_t>(value));
    }
    return evo::baselineSupported(b.uuid);
}
bool save(const Baseline& b) {
    int fd = open(journal, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    std::ostringstream text;
    text << "ENTRY_AA_TRIAL_V1\n"
         << b.pid << ' ' << b.identity << ' ' << b.instance << ' ' << b.interface << ' ' << hex(b.uuid) << '\n';
    std::string value = text.str();
    bool ok = write(fd, value.data(), value.size()) == static_cast<ssize_t>(value.size()) && fsync(fd) == 0;
    close(fd);
    return ok;
}
bool recoverySnapshot(evo::SppSnapshotReader& reader, unsigned instance, evo::SppSlotSnapshot& out,
                      bool allowSplit = false) {
    // Read-only settling time after a live SPP disconnect. No relaxation of the UUID/PID checks.
    uint32_t begin = evo::monoMs();
    do {
        if (reader.read(instance, out, allowSplit)) {
            return true;
        }
        usleep(100000);
    } while (evo::monoMs() - begin < 3000);
    ELOG("Supervisor", "restore: Alps reading not settled after 3 s: %s; journal kept", reader.error().c_str());
    return false;
}
// Giving our slot back races the phone. Its link dropped, the phone looks for the car again and, as long as our
// Android Auto record is there, can reconnect before a slot read every 100 ms ever sees it idle: the restore failed
// and every later start was refused (#88). The original pair
// cannot be written first: the stack answers a UUID change on a connected slot with status 7 and changes nothing
// (sppSetUUID, libevo_btappl.so), and only these SPP requests exist on its bus. So the slot is watched closely, the
// pair is written the moment it is idle, and our link is disconnected again when the phone was back first. Bounded:
// a few rounds within a time limit, and every ownership check stays as it was.
const useconds_t kSlotWatchUs = 10000;  // between two reads, each of which already takes 20 ms (two passes)
const uint32_t kIdleWaitMs = 1500;      // per disconnection request
const unsigned kRestoreRounds = 4;      // rounds (a disconnection while connected, then the write), at most
const uint32_t kRestoreLimitMs = 8000;  // no new round after this
// After a disconnection request: true once the slot reads idle. false after waitMs, or as soon as it is connected
// again after it was seen dropped: the phone was back before the slot settled.
bool waitIdle(evo::SppSnapshotReader& reader, unsigned slot, evo::SppSlotSnapshot& now, uint32_t waitMs) {
    const uint32_t begin = evo::monoMs();
    bool dropped = false;
    do {
        usleep(kSlotWatchUs);
        // Copies that disagree are allowed here: a refused write leaves them so, and the caller checks both.
        if (!reader.read(slot, now, true)) {
            continue;
        }
        if (now.idle()) {
            return true;
        }
        const bool linked = now.connected || now.state == 2;
        if (linked && dropped) {
            return false;
        }
        dropped = dropped || !linked;
    } while (evo::monoMs() - begin < waitMs);
    return false;
}
// Writes the original pair back over ours: now holds ours, the original or one of each, in either copy.
bool restoreUuid(const Baseline& b, evo::SppSnapshotReader& reader, evo::AlpsSppLink& bt, evo::SppSlotSnapshot& now) {
    const std::vector<uint8_t> aa = aaPayload();
    const uint32_t begin = evo::monoMs();
    for (unsigned round = 1;; ++round) {
        bool idle = now.idle();
        int result = 0;
        if (!idle) {
            // An unconfirmed partial registration does not establish ownership of a live connection.
            if (now.uuidPayload != aa) {
                ELOG("Supervisor",
                     "restore: SPP busy without a confirmed Android Auto registration: nothing disconnected");
                return false;
            }
            if (!bt.requestSppDisconnect()) {
                ELOG("Supervisor", "restore: Android Auto SPP disconnect request refused");
                return false;
            }
            idle = waitIdle(reader, b.instance, now, kIdleWaitMs);
        }
        if (idle) {
            if ((now.uuidPayload != aa && now.uuidPayload != b.uuid) ||
                (now.transportUuidPayload != aa && now.transportUuidPayload != b.uuid)) {
                ELOG("Supervisor", "restore: UUID changed during the disconnection: journal kept");
                return false;
            }
            // Pump fd may have seen data before DisconnectInd; close only our local bridge.
            bt.closeLink();
            result = bt.registerUuidPayload(b.uuid, 3000);
            if (result == 0) {
                return true;
            }
            // -1 with our link up: the phone reconnected before the request left. -1 otherwise: the stack never
            // confirmed, and this link refuses any further UUID: stop there.
            if (result < 0 && !bt.linkUp()) {
                ELOG("Supervisor", "restore: UUID restore not confirmed (result=%d)", result);
                return false;
            }
        }
        if (round >= kRestoreRounds || evo::monoMs() - begin >= kRestoreLimitMs) {
            if (idle) {
                ELOG("Supervisor", "restore: UUID restore not confirmed (result=%d) after %u rounds", result, round);
            } else {
                ELOG("Supervisor", "restore: disconnection of our SPP not confirmed after %u rounds", round);
            }
            return false;
        }
        if (idle) {
            ELOG("Supervisor", "restore: original UUID not taken (result=%d%s), round %u/%u: trying again", result,
                 result == 7 || bt.linkUp() ? ", the phone was back first" : "", round, kRestoreRounds);
        } else {
            ELOG("Supervisor", "restore: SPP %u not idle after the disconnection, round %u/%u: asking again",
                 b.instance, round, kRestoreRounds);
        }
        if (!recoverySnapshot(reader, b.instance, now, true)) {
            return false;
        }
    }
}
bool recover(const Baseline& b, evo::SppSnapshotReader& reader) {
    if (reader.pid() != b.pid || reader.identity() != b.identity) {
        ELOG("Supervisor", "restore: Alps restarted: journal kept, no old value written back");
        return false;
    }
    evo::SppSlotSnapshot now;
    if (!recoverySnapshot(reader, b.instance, now, true)) {
        return false;
    }
    ELOG("Supervisor", "restore: state before: SPP=%u transport=%u connected=%u", now.state, now.transportState,
         now.connected ? 1 : 0);
    const std::vector<uint8_t> aa = aaPayload();
    if ((now.uuidPayload != aa && now.uuidPayload != b.uuid) ||
        (now.transportUuidPayload != aa && now.transportUuidPayload != b.uuid)) {
        ELOG("Supervisor", "restore: third-party UUID found: nothing replaced");
        return false;
    }
    evo::AlpsSppLink bt;
    if (!bt.start(static_cast<uint8_t>(b.instance))) {
        ELOG("Supervisor", "restore: Alps bus unavailable");
        return false;
    }
    if ((now.uuidPayload == aa || now.transportUuidPayload == aa) && !restoreUuid(b, reader, bt, now)) {
        return false;
    }
    std::vector<uint8_t> cfg;
    // Slot 5's 0x6203 cannot be read back (slotConfigurationReadable): it is written back every time, confirmed by
    // the write's status, with the same conditions as a change seen on a readable slot.
    const bool readable = evo::slotConfigurationReadable(b.instance);
    if (readable && (!bt.readConfiguration(b.instance, 0x6203, cfg, 2000) || cfg.size() != 1 || cfg[0] > 1)) {
        ELOG("Supervisor", "restore: 0x6203 unreadable or invalid");
        return false;
    }
    if (!readable || cfg[0] != b.interface) {
        if (!recoverySnapshot(reader, b.instance, now) || !now.idle() || now.uuidPayload != b.uuid) {
            ELOG("Supervisor", "restore: original SPP busy or changed: interface restore postponed, journal kept");
            return false;
        }
        if (!bt.setDataInterface(b.interface, 2000)) {
            ELOG("Supervisor", "restore: 0x6203 restore not confirmed");
            return false;
        }
    }
    if (!recoverySnapshot(reader, b.instance, now) || now.uuidPayload != b.uuid) {
        ELOG("Supervisor", "restore: original UUID not read back after the restore");
        return false;
    }
    if (readable &&
        (!bt.readConfiguration(b.instance, 0x6203, cfg, 2000) || cfg.size() != 1 || cfg[0] != b.interface)) {
        ELOG("Supervisor", "restore: original 0x6203 not read back after the restore");
        return false;
    }
    ELOG("Supervisor", "restore: original UUID %s: restore confirmed (instance %u)",
         readable ? "and SPP interface read back" : "read back, SPP interface written back and confirmed", b.instance);
    evo::EvoIlm ilm;
    if (!ilm.surfaceAbsent(surface) || !ilm.detachSurface(surface, layer)) {
        ELOG("Supervisor", "restore: video removal not confirmed: journal kept");
        return false;
    }
    ELOG("Supervisor", "restore: our surface is gone; other surfaces kept");
    return unlink(journal) == 0;
}
// Why the BMW hotspot is missing (wland starts hostapd with the HMI and stops it after APConnectionTimeout
// without a client): hostapd control socket, /tmp/wland/ap.conf, wlan0 IPv4.
std::string apDiagnosis() {
    std::string s = access("/var/run/hostapd/wlan0", F_OK) == 0 ? "hostapd present" : "hostapd absent";
    s += access("/tmp/wland/ap.conf", F_OK) == 0 ? ", ap.conf present" : ", ap.conf absent";
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    std::string ip = "no address";
    if (fd >= 0) {
        ifreq rq;
        memset(&rq, 0, sizeof(rq));
        strcpy(rq.ifr_name, "wlan0");
        char b[INET_ADDRSTRLEN];
        if (ioctl(fd, SIOCGIFADDR, &rq) == 0 &&
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&rq.ifr_addr)->sin_addr, b, sizeof(b))) {
            ip = b;
        }
        ::close(fd);
    }
    return s + ", wlan0 " + ip;
}
// The panel as the compositor gives it, when one of the phone's canvases holds it. detail: the size, and why not.
bool readPanel(evo::EvoIlm& ilm, aa::DisplayPlan& display, std::string& detail) {
    uint32_t w = 0, h = 0;
    if (!ilm.screenSize(w, h)) {
        detail = "screen size unreadable";
        return false;
    }
    char size[32];
    snprintf(size, sizeof(size), "%ux%u", w, h);
    detail = size;
    if (!aa::planDisplay(w, h, display)) {
        detail += " larger than 1920x1080";
        return false;
    }
    return true;
}
// The screen half of the preflight, all a start over USB needs.
bool screenPreflight() {
    evo::EvoIlm ilm;
    aa::DisplayPlan display;
    std::string panel;
    if (!readPanel(ilm, display, panel)) {
        ELOG("Supervisor", "preflight: %s: no Bluetooth change", panel.c_str());
        return false;
    }
    if (!ilm.validateTarget(surface, layer, display.width, display.height)) {
        ELOG("Supervisor", "preflight: video target unavailable: no Bluetooth change");
        return false;
    }
    bool topFirst = false;
    if (!ilm.renderOrderTopFirst(topFirst)) {
        ELOG("Supervisor", "preflight: compositor stacking order unknown: no Bluetooth change");
        return false;
    }
    ELOG("Supervisor",
         "preflight: the compositor draws the first of a list %s; our surface goes %s the layer's BMW surfaces",
         topFirst ? "on top (genivi-flip-renderorder=1)" : "underneath", topFirst ? "first, above" : "last, above");
    return true;
}
// The SPP data interface of a slot (0x6203), read through a link that changes nothing.
bool slotInterface(unsigned slot, std::vector<uint8_t>& cfg) {
    evo::AlpsSppLink bt;
    bt.setMonitorOnly(true);
    const uint8_t id = static_cast<uint8_t>(slot);
    return bt.start(id) && bt.readConfiguration(id, 0x6203, cfg, 2000) && cfg.size() == 1 && cfg[0] <= 1;
}
// Slot 5 when BMW started it and nobody uses it (spp_baseline.hpp), slot 0 as before otherwise. Only reads.
bool chooseSlot(evo::SppSnapshotReader& reader, unsigned& slotId, evo::SppSlotSnapshot& slot,
                std::vector<uint8_t>& cfg) {
    evo::SppSlotSnapshot spare;
    const bool spareRead = reader.read(evo::kAaSlot, spare);
    if (spareRead) {
        ELOG("Supervisor", "preflight: SPP %u: state=%u transport=%u connected=%u UUID=%s", evo::kAaSlot, spare.state,
             spare.transportState, spare.connected ? 1 : 0, hex(spare.uuidPayload).c_str());
    } else {
        ELOG("Supervisor", "preflight: SPP %u unreadable: %s", evo::kAaSlot, reader.error().c_str());
    }
    evo::SlotOffer offer = evo::spareSlotOffer(spareRead, spare, aaPayload());
    if (offer == evo::kSlotBusy) {
        ELOG("Supervisor", "preflight: SPP %u connected or already holding Android Auto: start refused for now",
             evo::kAaSlot);
        return false;
    }
    if (offer == evo::kSlotFree) {
        // Its 0x6203 cannot be read through the stack (slotConfigurationReadable): BMW never sets it
        // (stack/etc/evo_config.xml sets slots 0 and 1), so its start value, 0, is saved and written back at the end.
        slotId = evo::kAaSlot;
        slot = spare;
        cfg.assign(1, 0);
        ELOG("Supervisor", "preflight: Android Auto takes SPP %u (free, generic pair); SPP 0 (BMW Apps) left alone",
             slotId);
        return true;
    }
    slotId = evo::kAaFallbackSlot;
    ELOG("Supervisor", "preflight: SPP %u not available on this car: Android Auto takes SPP %u, as before",
         evo::kAaSlot, slotId);
    if (!reader.read(slotId, slot)) {
        ELOG("Supervisor", "preflight: %s", reader.error().c_str());
        return false;
    }
    ELOG("Supervisor", "preflight: SPP %u: state=%u transport=%u connected=%u UUID=%s", slotId, slot.state,
         slot.transportState, slot.connected ? 1 : 0, hex(slot.uuidPayload).c_str());
    if (!slot.idle()) {
        ELOG("Supervisor", "preflight: instance %u busy or changing: start refused; instance 1 left alone", slotId);
        return false;
    }
    if (!evo::baselineSupported(slot.uuidPayload)) {
        ELOG("Supervisor", "preflight: unknown original UUID pair: start refused before any change; keep this log");
        return false;
    }
    if (!slotInterface(slotId, cfg)) {
        ELOG("Supervisor", "preflight: SPP %u interface (0x6203) unreadable or invalid: no change", slotId);
        return false;
    }
    return true;
}
bool preflight(evo::SppSnapshotReader& reader, Baseline& b) {
    if (!screenPreflight()) {
        return false;
    }
    reader.close();  // each attempt rescans Alps: with the first scan kept, a retry could never pass
    if (!reader.open()) {
        ELOG("Supervisor", "preflight: %s", reader.error().empty() ? "cannot open Alps" : reader.error().c_str());
        return false;
    }
    unsigned slotId = evo::kAaFallbackSlot;
    evo::SppSlotSnapshot slot;
    std::vector<uint8_t> cfg;
    if (!chooseSlot(reader, slotId, slot, cfg)) {
        return false;
    }
    ELOG("Supervisor", "preflight: UUID pair recognised; the SPP %u interface can be saved: 0x6203=%u%s", slotId,
         cfg[0], evo::slotConfigurationReadable(slotId) ? "" : " (start value; the stack cannot read it back)");
    aa::ApInfo ap;
    std::string source;
    if (!evo::loadAccessPoint(ap, source)) {
        ELOG("Supervisor", "preflight: BMW Wi-Fi hotspot unavailable (%s)", apDiagnosis().c_str());
        maybeRequestHotspot();
        return false;
    }
    b.pid = reader.pid();
    b.identity = reader.identity();
    b.instance = slotId;
    b.interface = cfg[0];
    b.uuid = slot.uuidPayload;
    ELOG("Supervisor", "preflight OK: target free, SPP %u idle, UUID can be saved, hotspot available", slotId);
    return true;
}
// --compat: what a start would find on this car, one line per point, for the compatibility check
// (docs/INSTALLATION.md). It only reads, like the service's preflight, and never asks for the hotspot.
void compatLine(const char* level, const char* id, const std::string& detail = std::string()) {
    printf("@%s|%s|%s\n", level, id, detail.c_str());
}
// The Bluetooth points (alps, uuid, spp) matter to wireless only: over USB the supervisor checks the screen alone and
// the service never opens the Alps stack. Without the USB file a failed point keeps its level and says that USB does
// not need it; with the file it is only information.
const char* bluetoothLevel(const char* level, bool usb, std::string& detail) {
    if (!strcmp(level, "ok")) {
        return level;
    }
    detail += detail.empty() ? "" : "; ";
    detail += usb ? "not used over USB" : "wireless only, USB mode does not use Bluetooth";
    return usb ? "info" : level;
}
void bluetoothLine(bool usb, const char* level, const char* id, std::string detail = std::string()) {
    level = bluetoothLevel(level, usb, detail);
    compatLine(level, id, detail);
}
// Functions found at run time (dlopen) escape the loader's own check.
std::string missingFunctions(const char* library, const char* const* names, size_t count) {
    evo::ElfFile elf;
    if (!elf.load(library)) {
        return library;
    }
    std::string missing;
    evo::ElfSymbol symbol;
    for (size_t i = 0; i < count; ++i) {
        if (!elf.symbol(names[i], symbol)) {
            missing += (missing.empty() ? "" : " ") + std::string(names[i]);
        }
    }
    return missing;
}
bool serviceRunning() {
    int fd = open(evo::contract::kWirelessLock, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    return held;
}
int compatReport() {
    alarm(60);
    // bmw::PluginApp: 2 + 34 slots that the iDrive app copies and patches, then the PluginAppHandler vtable.
    const uint32_t kPluginAppVtableSize = 172;
    evo::ElfFile framework;
    evo::ElfSymbol vtable;
    if (!framework.load("/opt/hmi/lib/libAppFramework.A.so") || !framework.symbol("_ZTVN3bmw9PluginAppE", vtable)) {
        compatLine("block", "abi", "bmw::PluginApp absent");
    } else if (vtable.size != kPluginAppVtableSize) {
        char detail[32];
        snprintf(detail, sizeof(detail), "size %u", vtable.size);
        compatLine("block", "abi", detail);
    } else {
        compatLine("ok", "abi");
    }
    const std::string vmostPath = std::string("/usr/local/lib/") + evo::kVmostLibrary;
    const std::string noVmost = missingFunctions(vmostPath.c_str(), evo::kVmostFunctions, evo::kVmostFunctionCount);
    compatLine(noVmost.empty() ? "ok" : "block", "vmost", noVmost);
    const char* const alsa[] = {"snd_pcm_open",    "snd_pcm_set_params", "snd_pcm_readi",
                                "snd_pcm_recover", "snd_pcm_close",      "snd_strerror"};
    const std::string noAlsa = missingFunctions("/usr/lib/libasound.so.2", alsa, sizeof(alsa) / sizeof(alsa[0]));
    compatLine(noAlsa.empty() ? "ok" : "warn", "mic", noAlsa);
    // The mode a start would use: the USB file, read as the service reads it.
    std::string usbPort;
    const int usbSetting = evo::readUsbSetting(evo::contract::kUsb, usbPort);
    if (usbSetting < 0) {
        compatLine("block", "usb", std::string(evo::contract::kUsb) + " holds something else than a USB port");
    } else if (usbSetting == 1) {
        compatLine("info", "usb", usbPort);
    }
    const bool usb = usbSetting == 1;
    if (serviceRunning()) {
        compatLine("info", "running");  // the screen and Bluetooth are in use: they say nothing about the car
        return 0;
    }
    evo::EvoIlm ilm;
    aa::DisplayPlan display;
    std::string panel;
    if (!readPanel(ilm, display, panel)) {
        compatLine("block", "video", panel);
    } else {
        const bool target = ilm.validateTarget(surface, layer, display.width, display.height);
        compatLine(target ? "ok" : "block", "video", target ? panel : panel + ": layer or surface");
        if (target && !display.triedInCar()) {
            compatLine("warn", "screen", panel + ": only 1280x480 and 800x480 have been tried in a car");
        }
    }
    bool topFirst = false;
    if (!ilm.renderOrderTopFirst(topFirst)) {
        compatLine("block", "order");
    }
    evo::SppSnapshotReader reader;
    evo::SppSlotSnapshot slot;
    if (!reader.open() || !reader.read(0, slot)) {
        bluetoothLine(usb, "block", "alps", reader.error());
    } else {
        bluetoothLine(usb, slot.idle() ? "ok" : "warn", "alps");
        bluetoothLine(usb, evo::baselineSupported(slot.uuidPayload) ? "ok" : "block", "uuid", hex(slot.uuidPayload));
        evo::AlpsSppLink bt;
        bt.setMonitorOnly(true);
        std::vector<uint8_t> cfg;
        const bool readable =
            bt.start(0) && bt.readConfiguration(0, 0x6203, cfg, 2000) && cfg.size() == 1 && cfg[0] <= 1;
        bluetoothLine(usb, readable ? "ok" : "block", "spp");
    }
    aa::ApInfo ap;
    std::string source;
    compatLine(evo::loadAccessPoint(ap, source) ? "ok" : "info", "hotspot");
    compatLine(evo::carGpsPresent() ? "ok" : "info", "gps");
    return 0;
}
// STOP, created there by the iDrive app: the service stops.
bool stopFilePresent() {
    return access(evo::contract::kServiceStop, F_OK) == 0;
}
// --service: working directory for the recovery journal and STOP (RAM; a reboot restores Bluetooth anyway).
bool enterServiceDir() {
    if (mkdir(evo::contract::kServiceDir, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    struct stat st;
    if (lstat(evo::contract::kServiceDir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) {
        return false;
    }
    return chdir(evo::contract::kServiceDir) == 0;
}
// The iDrive app hands us an append-only log as stdout: keep it bounded. On the data partition (kept_log.hpp): 512 KB,
// the previous 512 KB in the old file. In /tmp (RAM): emptied at 16 MB.
void boundLog() {
    const evo::KeptLogTrim trim =
        evo::trimKeptLog(1, evo::contract::kServiceLogKept, evo::contract::kServiceLogKeptOld, evo::kKeptLogMax);
    if (trim == evo::KeptLogTrim::kMoved) {
        ELOG("Supervisor", "log full (%ld KB): moved to %s", static_cast<long>(evo::kKeptLogMax / 1024),
             evo::contract::kServiceLogKeptOld);
    } else if (trim == evo::KeptLogTrim::kEmptied) {
        ELOG("Supervisor", "log full (%ld KB): emptied without a copy in %s",
             static_cast<long>(evo::kKeptLogMax / 1024), evo::contract::kServiceLogKeptOld);
    }
    struct stat st;
    if (trim == evo::KeptLogTrim::kNotKept && fstat(1, &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size > 16 * 1024 * 1024 && ftruncate(1, 0) == 0) {
        ELOG("Supervisor", "log emptied (16 MB reached)");
    }
}
}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--compat")) {
        return compatReport();
    }
    if (argc == 2 && !strcmp(argv[1], "--copy-logs")) {
        return evo::copyLogsToUsb();
    }
    if (argc == 2 && (!strcmp(argv[1], "--update-check") || !strcmp(argv[1], "--update-install"))) {
        return evo::updateFromUsb(!strcmp(argv[1], "--update-install"), ENTRY_AAD_VERSION);
    }
    if (argc != 2 || (!strcmp(argv[1], "--help"))) {
        puts(
            "Usage: aa-supervisor --service | --compat | --recover | --copy-logs | --update-check | --update-install\n"
            "  --service  started by the iDrive app: wireless Android Auto with no time limit, stopped by the app or\n"
            "             STOP; the BMW hotspot switched on as Apple CarPlay does when it is off. With the file\n"
            "             /var/local/data/AA/usb: Android Auto over a USB cable, Bluetooth and Wi-Fi untouched\n"
            "  --compat   what a start would find on this car, changing nothing\n"
            "  --recover  gives Bluetooth back after an unconfirmed stop (journal in the current folder)\n"
            "  --copy-logs  the app's and the service's logs onto the USB stick (exit 0 copied, 3 no stick, 4 no log,\n"
            "             5 write failed)\n"
            "  --update-check    what an update from the USB stick would do, changing nothing (report in\n"
            "             /var/local/data/AA/update-report.txt; exit codes: platform/update_install.hpp)\n"
            "  --update-install  installs the update signed by the project from the USB stick, then restarts the\n"
            "             unit (Android Auto must be stopped)");
        return argc == 2 ? 0 : 2;
    }
    const bool service = !strcmp(argv[1], "--service"), restore = !strcmp(argv[1], "--recover");
    if (!service && !restore) {
        return 2;
    }
    // The USB setting is read once: a service runs wireless or over USB from its start to its stop.
    std::string usbPort;
    const int usbSetting = service ? evo::readUsbSetting(evo::contract::kUsb, usbPort) : 0;
    if (usbSetting < 0) {
        ELOG("Supervisor", "%s holds something else than nothing or a USB port (1-1): start refused",
             evo::contract::kUsb);
        return 2;
    }
    const bool usb = usbSetting == 1;
    // The BMW hotspot is switched on as Apple CarPlay does, only for the iDrive app's wireless service.
    g_wlanAuto = service && !usb;
    signal(SIGINT, stopSignal);
    signal(SIGTERM, stopSignal);
    signal(SIGHUP, stopSignal);
    signal(SIGPIPE, SIG_IGN);
    // One supervisor at a time; --compat also reads this lock to know that Android Auto is running.
    int lock = open(evo::contract::kWirelessLock, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    if (lock < 0 || fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        flock(lock, LOCK_EX | LOCK_NB)) {
        ELOG("Supervisor", "a supervisor is already running, or its lock is invalid");
        return 1;
    }
    // Absolute daemon path for --service (started from HmiMain3D, any working directory).
    std::string daemon;
    if (service) {
        std::string self = argv[0];
        if (self.empty() || self[0] != '/') {
            ELOG("Supervisor", "absolute path required");
            return 2;
        }
        std::vector<char> copy(self.begin(), self.end());
        copy.push_back(0);
        daemon = std::string(dirname(&copy[0])) + "/entry-aad";
        if (!enterServiceDir()) {
            ELOG("Supervisor", "folder %s invalid", evo::contract::kServiceDir);
            return 1;
        }
        ELOG("Supervisor", "started by the iDrive app (pid %d); restore journal in %s", (int)getpid(),
             evo::contract::kServiceDir);
    }
    evo::SppSnapshotReader reader;
    Baseline baseline;
    if (service && access(journal, F_OK) == 0) {
        // A previous service of this boot ended without a confirmed restore: finish it before anything else.
        if (!load(baseline) || !reader.open() || !recover(baseline, reader)) {
            ELOG("Supervisor", "restore: previous restore not confirmed: service refused, journal kept");
            return 1;
        }
    }
    if (restore) {
        if (access(journal, F_OK) != 0 && errno == ENOENT) {
            puts("No Bluetooth restore pending.");
            return 0;
        }
        if (!load(baseline)) {
            ELOG("Supervisor", "restore: journal unreadable or invalid: nothing restored");
            return 1;
        }
        if (!reader.open()) {
            ELOG("Supervisor", "restore: cannot open Alps: %s; journal kept", reader.error().c_str());
            return 1;
        }
        return recover(baseline, reader) ? 0 : 1;
    }
    // At boot (autostart) the hotspot or Alps may not be ready yet: retry for up to 2 minutes.
    uint32_t t0 = evo::monoMs();
    while (!(usb ? screenPreflight() : preflight(reader, baseline))) {
        if (stopRequested || stopFilePresent()) {
            unlink(evo::contract::kServiceStop);
            ELOG("Supervisor", "stop asked during the preflight: nothing was changed");
            return 1;
        }
        if (evo::monoMs() - t0 >= 120000) {
            ELOG("Supervisor", "preflight still refused after 2 min: giving up, nothing was changed");
            return 1;
        }
        ELOG("Supervisor", "preflight refused: trying again in 5 s");
        for (int i = 0; i < 10 && !stopRequested && !stopFilePresent(); ++i) {
            usleep(500000);
        }
    }
    if (stopRequested || (!usb && !save(baseline))) {
        ELOG("Supervisor", "Alps state not saved (or saved incompletely), or a previous restore pending");
        return 1;
    }
    // Everything the service needs is on its command line: its mode, and the Alps state saved above, which it checks
    // again before registering the Android Auto UUID. Over USB Bluetooth is not touched: nothing was saved.
    std::vector<std::string> argList;
    argList.push_back(daemon);
    argList.push_back("--run");
    argList.push_back("--input");
    argList.push_back("forward");
    argList.push_back("--service");
    if (usb) {
        argList.push_back("--usb");
        if (!usbPort.empty()) {
            argList.push_back("--usb-port");
            argList.push_back(usbPort);
        }
    } else {
        char expectedPid[32], slot[8];
        snprintf(expectedPid, sizeof(expectedPid), "%d", baseline.pid);
        snprintf(slot, sizeof(slot), "%u", baseline.instance);
        argList.push_back("--configure-spp");
        argList.push_back("--spp-instance");
        argList.push_back(slot);
        argList.push_back("--alps-pid");
        argList.push_back(expectedPid);
        argList.push_back("--alps-start");
        argList.push_back(baseline.identity);
        argList.push_back("--uuid-pair");
        argList.push_back(hex(baseline.uuid));
    }
    std::vector<char*> args;
    for (size_t i = 0; i < argList.size(); ++i) {
        args.push_back(const_cast<char*>(argList[i].c_str()));
    }
    args.push_back(nullptr);
    posix_spawnattr_t attrs;
    int rc = posix_spawnattr_init(&attrs);
    pid_t child = -1;
    if (!rc) {
        rc = posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP);
        if (!rc) {
            rc = posix_spawnattr_setpgroup(&attrs, 0);
        }
        if (!rc) {
            rc = posix_spawn(&child, args[0], nullptr, &attrs, &args[0], environ);
        }
        posix_spawnattr_destroy(&attrs);
    }
    if (rc) {
        ELOG("Supervisor", "cannot start entry-aad: %s", strerror(rc));
    }
    int status = 0;
    bool ended = rc != 0, forced = false;
    uint32_t stopping = 0;
    ELOG("Supervisor",
         "%s Android Auto: service with no time limit (stopped by the iDrive app); iDrive inputs: controller and "
         "touchscreen forwarded to AA",
         usb ? "USB" : "wireless");
    uint32_t lastBound = evo::monoMs();
    while (!ended) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            ended = true;
            break;
        }
        if (done < 0 && errno != EINTR) {
            ELOG("Supervisor",
                 "restore: end of entry-aad not confirmed: journal kept, use --recover once it has stopped");
            return 1;
        }
        bool stop = stopRequested || stopFilePresent();
        if (evo::monoMs() - lastBound >= 5000) {
            lastBound = evo::monoMs();
            boundLog();
        }
        if (stop && !stopping) {
            stopping = evo::monoMs();
            kill(child, SIGTERM);
        }
        if (stopping && evo::monoMs() - stopping >= 10000) {
            kill(-child, SIGKILL);
            do {
                done = waitpid(child, &status, 0);
            } while (done < 0 && errno == EINTR);
            if (done != child) {
                ELOG("Supervisor", "restore: forced stop not confirmed: journal kept");
                return 1;
            }
            forced = true;
            break;
        }
        usleep(100000);
    }
    // The supervisor is separate from the video/session process: it also restores after a crash.
    bool restored = usb || recover(baseline, reader);  // over USB nothing was changed
    if (!restored) {
        ELOG("Supervisor", "restore: RESTORE NOT CONFIRMED: keep bluetooth-retour.txt, use --recover");
    }
    if (forced) {
        ELOG("Supervisor", "entry-aad killed: check that our surface is gone from the final listing");
    }
    unlink(evo::contract::kServiceStop);
    ELOG("Supervisor", "done (Bluetooth %s)",
         usb        ? "left untouched"
         : restored ? "restore confirmed"
                    : "restore NOT CONFIRMED");
    if (!restored || rc || forced || !WIFEXITED(status)) {
        return 1;
    }
    // entry-aad's 2 (command line refused) is passed on as is.
    return WEXITSTATUS(status) == 2 ? 2 : (WEXITSTATUS(status) == 0 ? 0 : 1);
}
