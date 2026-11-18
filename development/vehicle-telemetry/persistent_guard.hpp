// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <limits.h>
#include <sys/file.h>

#include "trial_guard.hpp"

namespace telemetry {
static const char PersistentToken[] = "vehicle-telemetry-146-persistent-v1\n";
class PersistentGuard {
public:
    PersistentGuard() : dir_(-1), lock_(-1), reason_("not-started") { memset(&record_, 0, sizeof(record_)); }
    ~PersistentGuard() { release(); }
    const char* reason() const { return reason_; }
    bool active() const { return dir_ >= 0 && lock_ >= 0; }
    static bool valid_boot(const char* s) {
        if (!s || strlen(s) != 36) {
            return false;
        }
        for (unsigned i = 0; i < 36; ++i) {
            bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            if (dash ? s[i] != '-' : !((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
                return false;
            }
        }
        return true;
    }
    static bool boot_id(char out[37]) {
        int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        char raw[37];
        ssize_t n = read(fd, raw, sizeof(raw));
        close(fd);
        if (n != 37 || raw[36] != '\n') {
            return false;
        }
        memcpy(out, raw, 36);
        out[36] = 0;
        return valid_boot(out);
    }
    bool start(const char* path, const char* boot, unsigned long pid) {
        release();
        reason_ = "guard-io";
        if (!valid_boot(boot) || !pid || pid > INT_MAX) {
            return false;
        }
        dir_ = trial_directory(path);
        if (dir_ < 0) {
            return false;
        }
        lock_ = openat(dir_, "persistent.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat st = {};
        if (lock_ < 0 || fstat(lock_, &st) || !safe(st) || flock(lock_, LOCK_EX | LOCK_NB)) {
            return deny("guard-locked-or-unsafe");
        }
        char raw[256];
        bool absent = false;
        if (!read_file("persistent.enabled", raw, sizeof(raw), absent) || absent || strcmp(raw, PersistentToken)) {
            return deny("not-enabled");
        }
        if (fstatat(dir_, "persistent.disabled", &st, AT_SYMLINK_NOFOLLOW) == 0) {
            return deny("disabled-after-failure");
        }
        if (errno != ENOENT) {
            return deny("disabled-state-unreadable");
        }
        Record previous = {};
        unsigned failures = 0;
        if (!read_record(previous, absent)) {
            return block("invalid-state");
        }
        if (!absent && !previous.clean) {
            if (!strcmp(previous.boot, boot) || !previous.healthy) {
                failures = previous.failures + 1;
            }
        }
        if (failures >= 2) {
            return block("repeated-unclean-starts");
        }
        memset(&record_, 0, sizeof(record_));
        strcpy(record_.boot, boot);
        record_.pid = pid;
        record_.failures = failures;
        if (!save()) {
            return block("cannot-record-start");
        }
        reason_ = "started";
        return true;
    }
    bool healthy() {
        if (!ours()) {
            return false;
        }
        record_.healthy = 1;
        if (!save()) {
            return block("cannot-record-healthy");
        }
        reason_ = "healthy";
        return true;
    }
    void clean() {
        if (!ours()) {
            return;
        }
        record_.clean = 1;
        if (!save()) {
            block("cannot-record-clean");
        } else {
            reason_ = "clean";
        }
    }
    void fail(const char* why) {
        if (dir_ >= 0) {
            block(why);
        }
    }
    void release() {
        if (lock_ >= 0) {
            close(lock_);
            lock_ = -1;
        }
        if (dir_ >= 0) {
            close(dir_);
            dir_ = -1;
        }
    }

private:
    struct Record {
        char boot[37];
        unsigned long pid;
        unsigned failures, healthy, clean;
    } record_;
    int dir_, lock_;
    const char* reason_;
    static bool safe(const struct stat& st) {
        return S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077) && st.st_nlink == 1;
    }
    bool deny(const char* why) {
        reason_ = why;
        release();
        return false;
    }
    bool read_file(const char* name, char* out, unsigned cap, bool& absent) {
        absent = false;
        int fd = openat(dir_, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            absent = errno == ENOENT;
            return absent;
        }
        struct stat st = {};
        bool ok = !fstat(fd, &st) && safe(st) && st.st_size > 0 && static_cast<unsigned long long>(st.st_size) < cap;
        if (ok) {
            ssize_t n = read(fd, out, cap - 1);
            ok = n == st.st_size;
            if (ok) {
                out[n] = 0;
            }
        }
        close(fd);
        return ok;
    }
    bool read_record(Record& value, bool& absent) {
        char raw[256];
        if (!read_file("persistent.state", raw, sizeof(raw), absent)) {
            return false;
        }
        if (absent) {
            return true;
        }
        int n = 0;
        if (sscanf(raw, "VT146P1 %36s %lu %u %u %u%n", value.boot, &value.pid, &value.failures, &value.healthy,
                   &value.clean, &n) != 5) {
            return false;
        }
        return !strcmp(raw + n, "\n") && valid_boot(value.boot) && value.pid && value.pid <= INT_MAX &&
               value.failures <= 2 && value.healthy <= 1 && value.clean <= 1;
    }
    bool save() {
        char raw[160];
        int n = snprintf(raw, sizeof(raw), "VT146P1 %s %lu %u %u %u\n", record_.boot, record_.pid, record_.failures,
                         record_.healthy, record_.clean);
        if (n <= 0 || n >= static_cast<int>(sizeof(raw))) {
            return false;
        }
        int fd = openat(dir_, "persistent.state.new", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            return false;
        }
        bool ok = write(fd, raw, n) == n && !fsync(fd);
        close(fd);
        if (ok) {
            ok = !renameat(dir_, "persistent.state.new", dir_, "persistent.state") && !fsync(dir_);
        }
        return ok;
    }
    bool ours() {
        if (dir_ < 0 || lock_ < 0) {
            return false;
        }
        Record current = {};
        bool absent = false;
        char enabled[64];
        bool missing = false;
        return read_file("persistent.enabled", enabled, sizeof(enabled), missing) && !missing &&
               !strcmp(enabled, PersistentToken) && read_record(current, absent) && !absent &&
               current.pid == record_.pid && !strcmp(current.boot, record_.boot);
    }
    bool block(const char* why) {
        reason_ = why;
        if (dir_ >= 0) {
            int fd = openat(dir_, "persistent.disabled", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd >= 0) {
                ssize_t ignored = write(fd, why, strlen(why));
                (void)ignored;
                fsync(fd);
                close(fd);
                fsync(dir_);
            }
        }
        release();
        return false;
    }
    PersistentGuard(const PersistentGuard&);
    PersistentGuard& operator=(const PersistentGuard&);
};
}  // namespace telemetry
