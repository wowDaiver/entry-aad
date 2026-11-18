// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace telemetry {
static const char TrialDirectory[] = "/var/local/data/VehicleTelemetry146";
static const char TrialToken[] = "vehicle-telemetry-146-single-start-v1\n";
inline int trial_directory(const char* path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    struct stat st = {};
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        close(fd);
        return -1;
    }
    return fd;
}
// Claim before native allocation/init. A crash after the claim cannot activate
// this candidate on the next HMI start. No root remount or firmware write.
inline bool consume_trial(const char* path) {
    int dir = trial_directory(path);
    if (dir < 0) {
        return false;
    }
    int fd = openat(dir, "trial-arm", O_RDONLY | O_NOFOLLOW);
    struct stat st = {}, claimed = {};
    char token[sizeof(TrialToken)] = {};
    bool ok = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077) &&
              st.st_nlink == 1 && st.st_size == sizeof(TrialToken) - 1 &&
              read(fd, token, sizeof(token)) == sizeof(TrialToken) - 1 && !strcmp(token, TrialToken);
    if (ok) {
        ok = !linkat(dir, "trial-arm", dir, "trial-consumed", 0);
    }
    if (ok) {
        ok = !fstatat(dir, "trial-consumed", &claimed, AT_SYMLINK_NOFOLLOW) && claimed.st_dev == st.st_dev &&
             claimed.st_ino == st.st_ino;
    }
    if (ok) {
        ok = !unlinkat(dir, "trial-arm", 0) && !fsync(dir);
    }
    if (fd >= 0) {
        close(fd);
    }
    close(dir);
    return ok;
}
inline void trial_log(const char* event, unsigned property = 999, const char* payload = 0,
                      const char* path = TrialDirectory) {
#if defined(TELEMETRY_EMULATOR_ONLY) || defined(TELEMETRY_HOST_ABI_STUB)
    (void)event;
    (void)property;
    (void)payload;
    (void)path;
#else
    int dir = trial_directory(path);
    if (dir < 0) {
        return;
    }
    int fd = openat(dir, "trial.log", O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0600);
    struct stat st = {};
    timespec ts = {};
    char line[512];
    if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() && st.st_nlink == 1 &&
        st.st_size >= 65536) {
        close(fd);
        fd = -1;
        struct stat previous = {};
        int rc = fstatat(dir, "trial.log.previous", &previous, AT_SYMLINK_NOFOLLOW);
        bool safe = rc == 0 ? (S_ISREG(previous.st_mode) && previous.st_uid == geteuid() && previous.st_nlink == 1)
                            : errno == ENOENT;
        if (safe && !renameat(dir, "trial.log", dir, "trial.log.previous")) {
            fd = openat(dir, "trial.log", O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        }
    }
    if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() && st.st_nlink == 1 &&
        st.st_size < 65536) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int n = snprintf(line, sizeof(line), "%ld pid=%ld event=%s property=%u %.320s\n", static_cast<long>(ts.tv_sec),
                         static_cast<long>(getpid()), event, property, payload ? payload : "");
        if (n > 0 && n < static_cast<int>(sizeof(line))) {
            ssize_t ignored = write(fd, line, n);
            (void)ignored;
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    close(dir);
#endif
}
}  // namespace telemetry
