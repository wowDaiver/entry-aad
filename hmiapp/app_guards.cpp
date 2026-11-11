// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The off switch, the crash guard, the boot-loop guard and the automatic start (app_guards.hpp).
#include "app_guards.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../platform/service_contract.hpp"
#include "app_log.hpp"
#include "service_link.hpp"

namespace evo {
namespace app {

namespace {

const long kAutostartDelayS = 30;
const int kBootGuardMax = 3;
const long kBootGuardWindowS = 15 * 60;
const int kCrashGuardMax = 3;  // starts without 2 min of uptime before the app turns itself off
const long kStableAfterS = 120;

bool g_autostartDone = false, g_stableMarked = false;

bool fileExists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}

// A setting that is a file: there or not. content: what a new file holds.
void setSwitch(const char* what, const char* path, bool on, const char* content) {
    if (!on) {
        if (unlink(path) != 0 && errno != ENOENT) {
            alog("%s: cannot remove %s (errno %d)", what, path, errno);
        }
        return;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        alog("%s: cannot create %s (errno %d)", what, path, errno);
        return;
    }
    ssize_t w = write(fd, content, strlen(content));
    (void)w;
    fsync(fd);
    close(fd);
}

bool bootGuardAllows() {
    time_t now = time(nullptr);
    long t[8];
    int n = 0;
    FILE* f = fopen(contract::kBootGuard, "r");
    if (f) {
        long v;
        while (n < 8 && fscanf(f, "%ld", &v) == 1) {
            if (v <= now && now - v < kBootGuardWindowS) {
                t[n++] = v;
            }
        }
        fclose(f);
    }
    if (n >= kBootGuardMax) {
        alog("autostart SKIPPED by the bootloop guard (%d starts within %ld min): use Start in the menu", n,
             kBootGuardWindowS / 60);
        return false;
    }
    f = fopen(contract::kBootGuard, "w");
    if (f) {
        for (int i = 0; i < n; ++i) {
            fprintf(f, "%ld\n", t[i]);
        }
        fprintf(f, "%ld\n", static_cast<long>(now));
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    return true;
}

}  // namespace

bool appDisabled() {
    if (fileExists(contract::kAppDisable)) {
        alog("disabled by %s", contract::kAppDisable);
        return true;
    }
    return false;
}

// ---- crash guard (HMI crash loop protection) ------------------------------------------------------------------------
// create() appends one line; 2 minutes later the status poll deletes the file. kCrashGuardMax lines = the HMI went
// down that many times shortly after loading us: create() then refuses (iDrive starts without the app).
bool crashGuardAllows() {
    int lines = 0;
    FILE* f = fopen(contract::kAppGuard, "r");
    if (f) {
        char b[64];
        while (fgets(b, sizeof(b), f)) {
            ++lines;
        }
        fclose(f);
    }
    if (lines >= kCrashGuardMax) {
        alog(
            "crash guard: the HMI restarted %d times shortly after loading this app: app turned off. "
            "Remove %s after checking the logs, then restart the unit.",
            kCrashGuardMax, contract::kAppGuard);
        return false;
    }
    f = fopen(contract::kAppGuard, "a");
    if (f) {
        fprintf(f, "%ld\n", static_cast<long>(time(nullptr)));
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    return true;
}

void resetGuardsWhenStable(time_t initTime) {
    if (!g_stableMarked && initTime && time(nullptr) - initTime >= kStableAfterS) {
        g_stableMarked = true;
        unlink(contract::kAppGuard);  // the HMI has been up 2 min with us: not a crash loop
        unlink(contract::kBootGuard);
        alog("stable for %ld s: crash and boot guards reset", kStableAfterS);
    }
}

// ---- start automatically (with a bootloop guard) --------------------------------------------------------------------
void serviceAutostart(time_t initTime) {
    if (g_autostartDone || !initTime || time(nullptr) - initTime < kAutostartDelayS) {
        return;
    }
    g_autostartDone = true;
    if (!autostartOn()) {
        return;
    }
    ServiceState d = readServiceState();
    if (d.pid != 0) {
        alog("autostart: service already running (pid %d)", d.pid);
        return;
    }
    if (!bootGuardAllows()) {
        return;
    }
    alog("autostart: launching the service");
    launchService();
}

void forgetBootGuard() {
    unlink(contract::kBootGuard);
}

bool autostartOn() {
    return fileExists(contract::kAutostart);
}

void toggleAutostart() {
    bool on = autostartOn();
    setSwitch("autostart", contract::kAutostart, !on, "1\n");
    alog("menu: start automatically %s -> %s", on ? "on" : "off", autostartOn() ? "on" : "off");
}

bool usbCableOn() {
    return fileExists(contract::kUsb);
}

void toggleUsbCable() {
    bool on = usbCableOn();
    setSwitch("USB cable", contract::kUsb, !on, "");  // empty: every USB port, as `touch` leaves it
    alog("menu: USB cable %s -> %s", on ? "on" : "off", usbCableOn() ? "on" : "off");
}

}  // namespace app
}  // namespace evo
