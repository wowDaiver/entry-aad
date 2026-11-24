// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The update from a USB stick on the car itself (update_install.hpp): /proc/mounts, the system partition remounted as
// the installation by hand does it, the car's own loader, the restart, and the locks. What this file decides from
// the car's answers (loaderFoundEverything, systemPartitionWritable, lockOwnFile) is in update_install.cpp, tested.
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "evo_log.hpp"
#include "evo_process.hpp"
#include "service_contract.hpp"
#include "update_install.hpp"
#include "update_key.hpp"

namespace evo {

namespace {

// The check of docs/INSTALLATION.md, step 2: the loader lists what a program would miss, without running any of it.
const char* const kLoaderEnv[] = {"LD_TRACE_LOADED_OBJECTS=1", "LD_WARN=yes", "LD_BIND_NOW=yes",
                                  "LD_LIBRARY_PATH=/usr/local/lib:/opt/hmi/lib:/usr/lib:/lib", nullptr};
const unsigned kRestartDelayS = 5;  // the menu says "restarting" meanwhile
// The iDrive app tries the supervisor's lock once a second to know whether Android Auto runs: an install waits out
// such a moment rather than answer "busy".
const unsigned kLockWaitMs = 2000;

std::string procMounts() {
    std::string mounts;
    const int fd = open("/proc/mounts", O_RDONLY | O_CLOEXEC);
    char buf[4096];
    ssize_t n;
    while (fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0 && mounts.size() < 256 * 1024) {
        mounts.append(buf, static_cast<size_t>(n));
    }
    if (fd >= 0) {
        close(fd);
    }
    return mounts;
}

class CarUpdateHost : public UpdateHost {
public:
    std::string mounts() override { return procMounts(); }
    bool systemWritable() override { return systemPartitionWritable(procMounts()); }
    bool setSystemWritable(bool writable) override {
        std::vector<std::string> args;
        args.push_back("-o");
        args.push_back(writable ? "remount,rw" : "remount,ro");
        args.push_back("/");
        std::string output;
        CommandReport report;
        if (!runCommand("/bin/mount", args, output, 30000, &report)) {
            ELOG("Update", "mount -o %s / refused (exit %d): %.200s", args[1].c_str(), report.exitCode,
                 report.text.c_str());
        }
        return systemWritable() == writable;
    }
    bool loads(const std::string& path, std::string& why) override {
        std::vector<std::string> args(1, path);
        std::string output;
        CommandReport report;
        runCommand(kCarLoader, args, output, 30000, &report, const_cast<char* const*>(kLoaderEnv));
        if (report.spawnError || report.timedOut || report.signal || report.truncated) {
            why = "the loader did not run to its end";
            return false;
        }
        return loaderFoundEverything(report.text, report.exitCode, why);
    }
    long long systemFreeBytes() override {
        struct statvfs fs;
        if (statvfs("/", &fs) != 0) {
            return -1;
        }
        return static_cast<long long>(fs.f_bavail) * static_cast<long long>(fs.f_frsize);
    }
    // The unit restarts as at every power-off (systemd), a few seconds later, from a child of its own: the iDrive app
    // reads this program's result meanwhile. The child keeps this program's locks until it runs the restart.
    void restartUnit() override {
        const pid_t pid = fork();
        if (pid == 0) {
            setsid();
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, nullptr);
            sleep(kRestartDelayS);
            sync();
            execl("/sbin/reboot", "reboot", static_cast<char*>(nullptr));
            execl("/bin/systemctl", "systemctl", "reboot", static_cast<char*>(nullptr));
            _exit(127);
        }
        if (pid < 0) {
            ELOG("Update", "cannot start the restart: restart the iDrive by hand");
        } else {
            ELOG("Update", "the unit restarts in %u s", kRestartDelayS);
        }
    }
    std::string stamp() override {
        char text[32];
        const time_t now = time(nullptr);
        struct tm local;
        localtime_r(&now, &local);
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
        return text;
    }
};

}  // namespace

UpdatePaths carUpdatePaths() {
    UpdatePaths paths;
    paths.pending = contract::kUpdatePending;
    paths.report = contract::kUpdateReport;
    paths.checked = contract::kUpdateChecked;
    paths.staging = contract::kUpdateStaging;
    paths.mediaPrefix = "/media/";
    return paths;
}

int updateFromUsb(bool install, const char* installedVersion) {
    signal(SIGPIPE, SIG_IGN);
    mkdir(contract::kDataDir, 0755);
    // Both locks last as long as this program (and its restart child): their descriptors stay open.
    if (lockOwnFile(contract::kUpdateLock, install ? kLockWaitMs : 0) < 0) {
        ELOG("Update", "another update from a USB stick runs");
        return static_cast<int>(UpdateResult::kBusy);
    }
    // The supervisor's own lock: no Android Auto starts during an install, none runs when it begins.
    if (install && lockOwnFile(contract::kWirelessLock, kLockWaitMs) < 0) {
        ELOG("Update", "Android Auto runs: stop it before the install");
        return static_cast<int>(UpdateResult::kBusy);
    }
    if (install) {
        // A few seconds the unit's shutdown can wait for: a signal must not stop the install between two files.
        sigset_t block;
        sigemptyset(&block);
        sigaddset(&block, SIGTERM);
        sigaddset(&block, SIGINT);
        sigaddset(&block, SIGHUP);
        sigprocmask(SIG_BLOCK, &block, nullptr);
    }
    CarUpdateHost host;
    Updater updater(host, carUpdatePaths(), trustedUpdateKeys(), installedVersion);
    const UpdateResult result = install ? updater.install() : updater.check();
    fputs(updater.report().c_str(), stdout);
    ELOG("Update", "%s: %s (report in %s)", install ? "install" : "check", updateResultText(result),
         contract::kUpdateReport);
    return static_cast<int>(result);
}

}  // namespace evo
