// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The logs onto a USB stick (platform/log_copy.hpp): which mount is a stick, and the copy into a temporary folder
// standing for it.
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../platform/log_copy.hpp"
#include "../platform/service_contract.hpp"

namespace {

std::string read(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    std::stringstream all;
    all << in.rdbuf();
    return all.str();
}

void put(const std::string& path, const std::string& text) {
    std::ofstream out(path.c_str(), std::ios::binary);
    out << text;
}

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

}  // namespace

int main() {
    // The car's mounts (/proc/mounts): the system, the data partition, the stick through Tuxera's FAT driver.
    const std::string car =
        "/dev/root / ext3 ro,relatime 0 0\n"
        "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n"
        "/dev/mmcblk0p9 /var/local ext4 rw,nosuid,nodev,noexec 0 0\n"
        "/dev/sda1 /media/sda1 tfat rw,nosuid,nodev,noexec,relatime,gid=1004,umask=002 0 0\n";
    assert(evo::findUsbStick(car) == "/media/sda1");
    assert(evo::findUsbStick("/dev/sdb1 /media/sdb1 texfat rw,noexec 0 0\n") == "/media/sdb1");
    assert(evo::findUsbStick("/dev/sdb1 /media/sdb1 tntfs rw 0 0\n") == "/media/sdb1");
    // Not a stick: read-only, another file system, outside /media, or /media itself.
    assert(evo::findUsbStick("/dev/sda1 /media/sda1 tfat ro,noexec 0 0\n").empty());
    assert(evo::findUsbStick("/dev/sda1 /media/sda1 ext4 rw 0 0\n").empty());
    assert(evo::findUsbStick("/dev/sda1 /mnt/sda1 vfat rw 0 0\n").empty());
    assert(evo::findUsbStick("tmpfs /media tmpfs rw 0 0\n").empty());
    assert(evo::findUsbStick("").empty() && evo::findUsbStick("garbage\n").empty());
    // The first stick wins.
    assert(evo::findUsbStick("/dev/sda1 /media/sda1 tfat ro 0 0\n/dev/sdb1 /media/sdb1 vfat rw 0 0\n") ==
           "/media/sdb1");
    // Every stick, read-only ones too, for the update that only reads them.
    const std::vector<evo::StickMount> sticks = evo::usbStickMounts(
        "/dev/sda1 /media/sda1 tfat ro,noexec 0 0\n/dev/sdb1 /media/sdb1 texfat rw,noexec 0 0\n"
        "/dev/sdc1 /mnt/sdc1 vfat rw 0 0\n/dev/sdd1 /media/sdd1 ext4 rw 0 0\n");
    assert(sticks.size() == 2 && sticks[0].point == "/media/sda1" && !sticks[0].writable);
    assert(sticks[1].point == "/media/sdb1" && sticks[1].writable);
    assert(evo::usbStickMounts(car).size() == 1 && evo::usbStickMounts("").empty());

    char dir[] = "/tmp/entry-aad-log-copy-XXXXXX";
    assert(mkdtemp(dir));
    const std::string base = dir, car_ = base + "/car", stick = base + "/stick";
    assert(mkdir(car_.c_str(), 0755) == 0 && mkdir(stick.c_str(), 0755) == 0);
    put(car_ + "/app.log", "app line\n");
    put(car_ + "/service.log", std::string(100000, 's'));
    put(car_ + "/version", "EntryEvo_ID5_1903\n");
    std::vector<evo::LogSource> sources;
    sources.push_back({car_ + "/app.log", "app.log", true});
    sources.push_back({car_ + "/app.old.log", "app.old.log", true});  // not on the car
    sources.push_back({car_ + "/service.log", "service.log", true});
    sources.push_back({car_ + "/version", "head-unit-version.txt", false});

    std::string folder;
    assert(evo::copyLogs(stick, sources, "20261122-183000", folder) == evo::LogCopyResult::kCopied);
    assert(folder == stick + "/EntryAA-logs/20261122-183000");
    assert(read(folder + "/app.log") == "app line\n" && read(folder + "/service.log").size() == 100000);
    assert(read(folder + "/head-unit-version.txt") == "EntryEvo_ID5_1903\n" && !exists(folder + "/app.old.log"));
    const std::string info = read(folder + "/info.txt");
    assert(info.find("20261122-183000") != std::string::npos);
    assert(info.find("app.old.log: not on the car") != std::string::npos);
    assert(info.find("service.log: 100000 bytes") != std::string::npos);
    // Copied again within the same second: a new folder, the first one untouched.
    std::string second;
    assert(evo::copyLogs(stick, sources, "20261122-183000", second) == evo::LogCopyResult::kCopied);
    assert(second == folder + "-2" && exists(folder + "/info.txt"));

    // Only the version on the car: no log to send.
    std::vector<evo::LogSource> versionOnly(1, sources[3]);
    versionOnly.push_back({car_ + "/missing.log", "missing.log", true});
    assert(evo::copyLogs(stick, versionOnly, "20261122-183100", folder) == evo::LogCopyResult::kNothingToCopy);

    // A stick that refuses the folder: write-protected (unless the test runs as root, which writes anyway).
    if (geteuid() != 0) {
        const std::string locked = base + "/locked";
        assert(mkdir(locked.c_str(), 0555) == 0);
        assert(evo::copyLogs(locked, sources, "20261122-183200", folder) == evo::LogCopyResult::kWriteFailed);
    }
    // No such stick at all.
    assert(evo::copyLogs(base + "/gone", sources, "20261122-183300", folder) == evo::LogCopyResult::kWriteFailed);

    // What the car offers: the logs, the report of the last update from a USB stick, then the version, never anything
    // else.
    const std::vector<evo::LogSource> carSources = evo::carLogSources();
    assert(carSources.size() >= 6 && carSources.back().path == "/etc/version" && !carSources.back().log);
    const evo::LogSource& report = carSources[carSources.size() - 2];
    assert(report.path == evo::contract::kUpdateReport && report.name == "update-report.txt" && !report.log);
    for (size_t i = 0; i + 2 < carSources.size(); ++i) {
        assert(carSources[i].log && carSources[i].path.find(".log") != std::string::npos);
    }

    const std::string clean = "chmod -R u+w '" + base + "' && rm -rf '" + base + "'";
    assert(system(clean.c_str()) == 0);
    puts(
        "PASS logs onto a USB stick: the car's stick in /proc/mounts, copy with info.txt, a second folder, no log, "
        "a stick that refuses");
    return 0;
}
