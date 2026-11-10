// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Wireless Android Auto supervisor, started by the iDrive app (hmiapp/) with --service: prepares Bluetooth and Wi-Fi,
// runs entry-aad, gives Bluetooth back on exit. --compat rehearses a start for the compatibility check, --recover
// finishes a restore left unconfirmed. Never restarts BMW services, never edits firmware.
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
#include "evo_ap.hpp"
#include "evo_elf.hpp"
#include "evo_gps.hpp"
#include "evo_ilm.hpp"
#include "evo_log.hpp"
#include "evo_spp_snapshot.hpp"
#include "evo_wlan.hpp"
#include "service_contract.hpp"
#include "spp_baseline.hpp"
#include "transport_policy.hpp"
#include "usb_transport.hpp"
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
        b.pid <= 0 || b.instance != 0 || b.interface > 1 || b.identity.empty() ||
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
    if (now.uuidPayload == aa || now.transportUuidPayload == aa) {
        if (!now.idle()) {
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
            uint32_t begin = evo::monoMs();
            bool idle = false;
            do {
                usleep(100000);
                if (reader.read(b.instance, now) && now.idle()) {
                    idle = true;
                    break;
                }
            } while (evo::monoMs() - begin < 5000);
            if (!idle) {
                ELOG("Supervisor", "restore: disconnection of our SPP not confirmed");
                return false;
            }
        }
        if ((now.uuidPayload != aa && now.uuidPayload != b.uuid) ||
            (now.transportUuidPayload != aa && now.transportUuidPayload != b.uuid)) {
            ELOG("Supervisor", "restore: UUID changed during the disconnection: journal kept");
            return false;
        }
        // Pump fd may have seen data before DisconnectInd; close only our local bridge.
        bt.closeLink();
        int result = bt.registerUuidPayload(b.uuid, 3000);
        if (result != 0) {
            ELOG("Supervisor", "restore: UUID restore not confirmed (result=%d)", result);
            return false;
        }
    }
    std::vector<uint8_t> cfg;
    if (!bt.readConfiguration(b.instance, 0x6203, cfg, 2000) || cfg.size() != 1 || cfg[0] > 1) {
        ELOG("Supervisor", "restore: 0x6203 unreadable or invalid");
        return false;
    }
    if (cfg[0] != b.interface) {
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
    if (!bt.readConfiguration(b.instance, 0x6203, cfg, 2000) || cfg.size() != 1 || cfg[0] != b.interface) {
        ELOG("Supervisor", "restore: original 0x6203 not read back after the restore");
        return false;
    }
    ELOG("Supervisor", "restore: original UUID and SPP interface read back: restore confirmed (instance %u)",
         b.instance);
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
bool preflight(evo::SppSnapshotReader& reader, Baseline& b) {
    evo::EvoIlm ilm;
    if (!ilm.validateTarget(surface, layer, 1280, 480)) {
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
    reader.close();  // each attempt rescans Alps: with the first scan kept, a retry could never pass
    if (!reader.open()) {
        ELOG("Supervisor", "preflight: %s", reader.error().empty() ? "cannot open Alps" : reader.error().c_str());
        return false;
    }
    evo::SppSlotSnapshot slot;
    if (!reader.read(0, slot)) {
        ELOG("Supervisor", "preflight: %s", reader.error().c_str());
        return false;
    }
    ELOG("Supervisor", "preflight: SPP 0: state=%u transport=%u connected=%u UUID=%s", slot.state, slot.transportState,
         slot.connected ? 1 : 0, hex(slot.uuidPayload).c_str());
    if (!slot.idle()) {
        ELOG("Supervisor", "preflight: instance 0 busy or changing: start refused; instance 1 left alone");
        return false;
    }
    if (!evo::baselineSupported(slot.uuidPayload)) {
        ELOG("Supervisor", "preflight: unknown original UUID pair: start refused before any change; keep this log");
        return false;
    }
    evo::AlpsSppLink bt;
    bt.setMonitorOnly(true);
    std::vector<uint8_t> cfg;
    if (!bt.start(0) || !bt.readConfiguration(0, 0x6203, cfg, 2000) || cfg.size() != 1 || cfg[0] > 1) {
        ELOG("Supervisor", "preflight: SPP 0 interface (0x6203) unreadable or invalid: no change");
        return false;
    }
    ELOG("Supervisor", "preflight: UUID pair recognised; the SPP 0 interface can be saved: 0x6203=%u", cfg[0]);
    aa::ApInfo ap;
    std::string source;
    if (!evo::loadAccessPoint(ap, source)) {
        ELOG("Supervisor", "preflight: BMW Wi-Fi hotspot unavailable (%s)", apDiagnosis().c_str());
        maybeRequestHotspot();
        return false;
    }
    b.pid = reader.pid();
    b.identity = reader.identity();
    b.instance = 0;
    b.interface = cfg[0];
    b.uuid = slot.uuidPayload;
    ELOG("Supervisor", "preflight OK: target free, SPP 0 idle, UUID can be saved, hotspot available");
    return true;
}
// --compat: what a start would find on this car, one line per point, for the compatibility check
// (docs/INSTALLATION.md). It only reads, like the service's preflight, and never asks for the hotspot.
void compatLine(const char* level, const char* id, const std::string& detail = std::string()) {
    printf("@%s|%s|%s\n", level, id, detail.c_str());
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
    if (serviceRunning()) {
        compatLine("info", "running");  // the screen and Bluetooth are in use: they say nothing about the car
        return 0;
    }
    evo::EvoIlm ilm;
    compatLine(ilm.validateTarget(surface, layer, 1280, 480) ? "ok" : "block", "video");
    bool topFirst = false;
    if (!ilm.renderOrderTopFirst(topFirst)) {
        compatLine("block", "order");
    }
    evo::SppSnapshotReader reader;
    evo::SppSlotSnapshot slot;
    if (!reader.open() || !reader.read(0, slot)) {
        compatLine("block", "alps", reader.error());
    } else {
        compatLine(slot.idle() ? "ok" : "warn", "alps");
        compatLine(evo::baselineSupported(slot.uuidPayload) ? "ok" : "block", "uuid", hex(slot.uuidPayload));
        evo::AlpsSppLink bt;
        bt.setMonitorOnly(true);
        std::vector<uint8_t> cfg;
        const bool readable =
            bt.start(0) && bt.readConfiguration(0, 0x6203, cfg, 2000) && cfg.size() == 1 && cfg[0] <= 1;
        compatLine(readable ? "ok" : "block", "spp");
    }
    aa::ApInfo ap;
    std::string source;
    compatLine(evo::loadAccessPoint(ap, source) ? "ok" : "info", "hotspot");
    compatLine(evo::carGpsPresent() ? "ok" : "info", "gps");
    return 0;
}
// STOP, created there by the iDrive app: the service stops.
const char* stopPath = evo::contract::kServiceStop;
bool stopFilePresent() {
    return access(stopPath, F_OK) == 0;
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
// The iDrive app hands us an append-only log as stdout: keep it bounded (RAM).
void boundLog() {
    struct stat st;
    if (fstat(1, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 16 * 1024 * 1024 && ftruncate(1, 0) == 0) {
        ELOG("Supervisor", "log emptied (16 MB reached)");
    }
}
int selectAndRunService(const std::string& daemon, int lock, int usbConfig, const aa::UsbSelection& usbSelection,
                        const char* sysfs = "/sys/bus/usb/devices", unsigned stopGraceMs = 10000) {
    const bool automatic = usbConfig == 1 && usbSelection.automatic;
    bool usb = usbConfig == 1;
    if (automatic) {
        const evo::PhonePresence presence = evo::phonePresence(usbSelection, sysfs);
        if (presence == evo::kPresenceUnknown) {
            ELOG("Supervisor", "selected USB port unreadable: automatic start refused");
            return 1;
        }
        usb = evo::startupTransport(presence) == evo::kUsbProjection;
        ELOG("Supervisor", "auto transport at startup: %s; cable changes apply only after Stop/Start",
             usb ? "USB" : "wireless");
    }
    g_wlanAuto = !usb;  // Only the BMW access point is requested; the phone hotspot is never changed.
    evo::SppSnapshotReader reader;
    Baseline baseline = {};
    // At boot (autostart) the hotspot or Alps may not be ready yet: retry for up to 2 minutes.
    uint32_t t0 = evo::monoMs();
    while (true) {
        bool ready = false;
        if (usb) {
            evo::EvoIlm ilm;
            bool topFirst = false;
            ready = ilm.validateTarget(surface, layer, 1280, 480) && ilm.renderOrderTopFirst(topFirst);
        } else {
            ready = preflight(reader, baseline);
        }
        if (ready) {
            break;
        }
        if (stopRequested || stopFilePresent()) {
            unlink(stopPath);
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
    // again before registering the Android Auto UUID.
    char expectedPid[32];
    snprintf(expectedPid, sizeof(expectedPid), "%d", baseline.pid);
    std::vector<std::string> argList;
    argList.push_back(daemon);
    argList.push_back("--run");
    argList.push_back("--input");
    argList.push_back("forward");
    argList.push_back("--service");
    if (usb) {
        char vid[16], pid[16], lockNumber[16];
        snprintf(vid, sizeof(vid), "0x%x", usbSelection.vendor);
        snprintf(pid, sizeof(pid), "0x%x", usbSelection.product);
        snprintf(lockNumber, sizeof(lockNumber), "%d", lock);
        argList.push_back("--usb-port");
        argList.push_back(usbSelection.port);
        argList.push_back("--usb-vid");
        argList.push_back(vid);
        argList.push_back("--usb-pid");
        argList.push_back(pid);
        if (automatic) {
            // The automatic profile explicitly selects this physical connector. Never detach a BMW owner.
            argList.push_back("--usb-adopt-accessory");
        }
        argList.push_back("--usb-lock-fd");
        argList.push_back(lockNumber);
        // Share the same open-file-description lock with the child. The daemon verifies the inode and flock.
        if (fcntl(lock, F_SETFD, 0)) {
            return 1;
        }
    } else {
        argList.push_back("--configure-spp");
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
    if (usb) {
        fcntl(lock, F_SETFD, FD_CLOEXEC);
    }
    if (rc) {
        ELOG("Supervisor", "cannot start entry-aad: %s", strerror(rc));
    }
    int status = 0;
    bool ended = rc != 0, forced = false;
    uint32_t stopping = 0;
    ELOG("Supervisor",
         "%s Android Auto: service with no time limit (stopped by the iDrive app); iDrive inputs: controller "
         "and touchscreen forwarded to AA",
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
        if (stopping && evo::monoMs() - stopping >= stopGraceMs) {
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
    bool restored = usb || recover(baseline, reader);
    if (!restored) {
        ELOG("Supervisor", "restore: RESTORE NOT CONFIRMED: keep bluetooth-retour.txt, use --recover");
    }
    if (forced) {
        ELOG("Supervisor", "entry-aad killed: check that our surface is gone from the final listing");
    }
    unlink(stopPath);
    ELOG("Supervisor", "done (Bluetooth %s)",
         usb ? "unchanged by USB mode" : (restored ? "restored" : "RESTORE NOT CONFIRMED"));
    if (!restored || rc || forced || !WIFEXITED(status)) {
        return 1;
    }
    // entry-aad's 2 (command line refused) is passed on as is.
    return WEXITSTATUS(status) == 2 ? 2 : (WEXITSTATUS(status) == 0 ? 0 : 1);
}
}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--compat")) {
        return compatReport();
    }
    if (argc != 2 || (!strcmp(argv[1], "--help"))) {
        puts(
            "Usage: aa-supervisor --service | --compat | --recover\n"
            "  --service  iDrive service; [auto ]PORT VID PID in usb-phone selects auto or USB-only; absent = "
            "wireless\n"
            "             auto chooses once at startup; changing the cable does not switch a running service\n"
            "  --compat   what a start would find on this car, changing nothing\n"
            "  --recover  gives Bluetooth back after an unconfirmed stop (journal in the current folder)");
        return argc == 2 ? 0 : 2;
    }
    const bool service = !strcmp(argv[1], "--service"), restore = !strcmp(argv[1], "--recover");
    setenv("XDG_RUNTIME_DIR", "/run/system/0", 0);
    setenv("WAYLAND_DISPLAY", "wayland-0", 0);
    aa::UsbSelection usbSelection;
    const int usbConfig = service ? usbSelection.loadFile("/var/local/data/AA/usb-phone") : 0;
    if (usbConfig < 0) {
        ELOG("Supervisor", "USB opt-in file is unsafe or invalid; start refused");
        return 2;
    }
    if (!service && !restore) {
        return 2;
    }
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
    Baseline baseline = {};
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
    return selectAndRunService(daemon, lock, usbConfig, usbSelection);
}
