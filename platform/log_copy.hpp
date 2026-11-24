// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The logs copied to a USB stick from the iDrive menu: sending them needs neither a PC nor SSH.
//
// The car mounts a USB stick under /media (udev: /etc/udev/scripts/mount.sh, then /usr/bin/my-mount.sh): FAT, exFAT
// or NTFS through Tuxera's tfat, texfat and tntfs, read-write, noexec. The iDrive app runs `aa-supervisor
// --copy-logs`, which finds the first such stick in /proc/mounts and copies into EntryAA-logs/<date> on it what there
// is of the app's and the service's logs, the head unit's software version (/etc/version) and an info.txt listing
// them. It only copies: nothing is deleted or changed on the car. The logs hold no password and no Wi-Fi or Bluetooth
// address: the programs never write them. tests/test_log_copy.cpp.
#ifndef ENTRY_AAD_PLATFORM_LOG_COPY_HPP
#define ENTRY_AAD_PLATFORM_LOG_COPY_HPP

#include <string>
#include <vector>

namespace evo {

// Also the exit codes of `aa-supervisor --copy-logs`, read by the iDrive app.
enum class LogCopyResult : int {
    kCopied = 0,
    kNoStick = 3,        // no USB stick mounted read-write under /media
    kNothingToCopy = 4,  // no log on the car (the "keep logs" box was never ticked)
    kWriteFailed = 5,    // the stick refused a folder or a file (full, write-protected, pulled out)
};

// The USB sticks in mounts (/proc/mounts) under prefix, mounted with a file system the car mounts sticks with, in
// their order there.
struct StickMount {
    std::string point;
    bool writable;
};
std::vector<StickMount> usbStickMounts(const std::string& mounts, const std::string& prefix = "/media/");
// The mount point of the first of them mounted read-write. Empty when there is none.
std::string findUsbStick(const std::string& mounts, const std::string& prefix = "/media/");

struct LogSource {
    std::string path;  // on the car
    std::string name;  // on the stick
    bool log;          // a log, rather than the version: at least one must be found
};
// What the car has: the app's log and its old part, the service's kept log and its old part, the service's log in
// /tmp when it is a file (the data partition refused it) rather than the link, the report of the last update from a
// USB stick (update_install.hpp), /etc/version.
std::vector<LogSource> carLogSources();

// Copies the sources found into <stick>/EntryAA-logs/<stamp> (another name if it exists), with info.txt: the stamp,
// and each source copied with its size or why not. folder: the folder created on the stick.
LogCopyResult copyLogs(const std::string& stick, const std::vector<LogSource>& sources, const std::string& stamp,
                       std::string& folder);

// --copy-logs on the car: the stick from /proc/mounts, the car's sources, the date as the stamp. Logs one line, the
// result as the exit code.
int copyLogsToUsb();

}  // namespace evo

#endif  // ENTRY_AAD_PLATFORM_LOG_COPY_HPP
