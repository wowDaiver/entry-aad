// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The logs copied to a USB stick (log_copy.hpp).
#include "log_copy.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sstream>

#include "evo_log.hpp"
#include "service_contract.hpp"

namespace evo {

namespace {

const char kFolder[] = "EntryAA-logs";
// The kept logs stay under 1 MB; the /tmp log of an older start can reach 16 MB before the supervisor empties it.
const off_t kSourceMax = 32 * 1024 * 1024;

bool stickFileSystem(const std::string& type) {
    static const char* const kTypes[] = {"tfat", "texfat", "tntfs", "vfat", "exfat", "ntfs", "msdos"};
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); ++i) {
        if (type == kTypes[i]) {
            return true;
        }
    }
    return false;
}

std::string number(long long value) {
    char text[24];
    snprintf(text, sizeof(text), "%lld", value);
    return text;
}

bool writeAll(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t w = write(fd, data, size);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return false;
        }
        data += w;
        size -= static_cast<size_t>(w);
    }
    return true;
}

// One source into the folder, flushed to the stick. False when the stick refused it; *size: the bytes copied.
bool copyFile(int in, const std::string& to, off_t* size) {
    const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        return false;
    }
    char buf[16384];
    bool ok = true;
    *size = 0;
    while (ok && *size < kSourceMax) {
        const ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        ok = writeAll(out, buf, static_cast<size_t>(n));
        *size += n;
    }
    ok = fsync(out) == 0 && ok;
    return close(out) == 0 && ok;
}

}  // namespace

std::vector<StickMount> usbStickMounts(const std::string& mounts, const std::string& prefix) {
    std::vector<StickMount> sticks;
    std::istringstream lines(mounts);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string device, point, type, options;
        if (!(fields >> device >> point >> type >> options)) {
            continue;
        }
        if (point.size() > prefix.size() && point.compare(0, prefix.size(), prefix) == 0 && stickFileSystem(type)) {
            const StickMount stick = {point, options == "rw" || options.compare(0, 3, "rw,") == 0};
            sticks.push_back(stick);
        }
    }
    return sticks;
}

std::string findUsbStick(const std::string& mounts, const std::string& prefix) {
    const std::vector<StickMount> sticks = usbStickMounts(mounts, prefix);
    for (size_t i = 0; i < sticks.size(); ++i) {
        if (sticks[i].writable) {
            return sticks[i].point;
        }
    }
    return std::string();
}

std::vector<LogSource> carLogSources() {
    std::vector<LogSource> sources;
    sources.push_back({contract::kAppLog, "app.log", true});
    sources.push_back({contract::kAppLogOld, "app.old.log", true});
    sources.push_back({contract::kServiceLogKept, "service.log", true});
    sources.push_back({contract::kServiceLogKeptOld, "service.old.log", true});
    struct stat ram;
    if (lstat(contract::kServiceLog, &ram) == 0 && S_ISREG(ram.st_mode)) {
        sources.push_back({contract::kServiceLog, "service-tmp.log", true});
    }
    sources.push_back({contract::kUpdateReport, "update-report.txt", false});
    sources.push_back({"/etc/version", "head-unit-version.txt", false});
    return sources;
}

LogCopyResult copyLogs(const std::string& stick, const std::vector<LogSource>& sources, const std::string& stamp,
                       std::string& folder) {
    const std::string parent = stick + "/" + kFolder;
    if (mkdir(parent.c_str(), 0755) != 0 && errno != EEXIST) {
        return LogCopyResult::kWriteFailed;
    }
    folder.clear();
    for (int n = 1; n <= 9 && folder.empty(); ++n) {
        const std::string name = parent + "/" + stamp + (n > 1 ? "-" + number(n) : std::string());
        if (mkdir(name.c_str(), 0755) == 0) {
            folder = name;
        } else if (errno != EEXIST) {
            return LogCopyResult::kWriteFailed;
        }
    }
    if (folder.empty()) {
        return LogCopyResult::kWriteFailed;
    }
    std::string info = "EntryNav2 Android Auto logs, " + stamp + "\n";
    bool anyLog = false, failed = false;
    for (size_t i = 0; i < sources.size(); ++i) {
        const LogSource& s = sources[i];
        const int in = open(s.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (in < 0) {
            info += s.name + ": not on the car (" + s.path + ")\n";
            continue;
        }
        off_t size = 0;
        const bool copied = copyFile(in, folder + "/" + s.name, &size);
        close(in);
        if (!copied) {
            failed = true;
            info += s.name + ": the stick refused it\n";
            continue;
        }
        anyLog = anyLog || s.log;
        info += s.name + ": " + number(static_cast<long long>(size)) + " bytes from " + s.path + "\n";
    }
    const int out = open((folder + "/info.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    const bool infoWritten = out >= 0 && writeAll(out, info.data(), info.size()) && fsync(out) == 0;
    if (out >= 0) {
        close(out);
    }
    if (failed || !infoWritten) {
        return LogCopyResult::kWriteFailed;
    }
    return anyLog ? LogCopyResult::kCopied : LogCopyResult::kNothingToCopy;
}

int copyLogsToUsb() {
    std::string mounts;
    {
        const int fd = open("/proc/mounts", O_RDONLY | O_CLOEXEC);
        char buf[4096];
        ssize_t n;
        while (fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0 && mounts.size() < 256 * 1024) {
            mounts.append(buf, static_cast<size_t>(n));
        }
        if (fd >= 0) {
            close(fd);
        }
    }
    const std::string stick = findUsbStick(mounts);
    if (stick.empty()) {
        ELOG("LogCopy", "no USB stick mounted read-write under /media: nothing copied");
        return static_cast<int>(LogCopyResult::kNoStick);
    }
    char stamp[32];
    const time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    std::string folder;
    const LogCopyResult result = copyLogs(stick, carLogSources(), stamp, folder);
    sync();  // the driver may pull the stick out as soon as the menu says so
    ELOG("LogCopy", "logs to %s: %s", folder.empty() ? stick.c_str() : folder.c_str(),
         result == LogCopyResult::kCopied          ? "copied"
         : result == LogCopyResult::kNothingToCopy ? "no log on the car"
                                                   : "the stick refused a folder or a file");
    return static_cast<int>(result);
}

}  // namespace evo
