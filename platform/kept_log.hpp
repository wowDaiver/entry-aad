// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The service's log kept across the car's sleep, in the maintenance profile only.
//
// The service used to write into /tmp, which is gone when the car sleeps: the log of a drive was lost before anyone
// could fetch it (#101, #88). In the maintenance profile it now goes to the data partition, bounded: at most
// kKeptLogMax in the current file and as much in the old one. BMW's own settings live on that partition too: a log
// must never fill it.
// - The iDrive app opens it for the supervisor and the service (openServiceLog). A full current file first becomes
//   the old one; then it is opened in append mode. The /tmp name becomes a link to it, so what reads the log there
//   (scp, the installer's report) still finds it. Should the data partition refuse it, the log goes to /tmp as before.
// - While they run, the supervisor checks it every 5 s on its own stdout (trimKeptLog): over kKeptLogMax, the content
//   is copied to the old file and the current one emptied. The writers keep their descriptors: in append mode, their
//   next lines start the emptied file. Lines written during the copy itself can be lost. Should the copy fail, the
//   log is emptied all the same: bounded first.
// Without the maintenance profile, nothing is written: /dev/null. tests/test_kept_log.cpp.
#ifndef ENTRY_AAD_PLATFORM_KEPT_LOG_HPP
#define ENTRY_AAD_PLATFORM_KEPT_LOG_HPP

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace evo {

const off_t kKeptLogMax = 512 * 1024;

// What fd holds, from its start, into path, through a temporary file renamed over it. False on any error, path then
// left as it was.
inline bool copyToFile(int fd, const char* path) {
    char tmp[256];
    const int len = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (len < 0 || len >= static_cast<int>(sizeof(tmp))) {
        return false;
    }
    const int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        return false;
    }
    char buf[16384];
    off_t at = 0;
    bool ok = true;
    while (ok) {
        const ssize_t n = pread(fd, buf, sizeof(buf), at);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        at += n;
        for (ssize_t done = 0; ok && done < n;) {
            const ssize_t w = write(out, buf + done, static_cast<size_t>(n - done));
            if (w < 0 && errno == EINTR) {
                continue;
            }
            ok = w > 0;
            done += w > 0 ? w : 0;
        }
    }
    ok = close(out) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

// The iDrive app, before it starts the supervisor: where their output goes. marker: the maintenance profile's file;
// kept, keptOld: the log on the data partition and its old part; ram: the name in /tmp. The descriptor (append mode,
// close-on-exec), or -1; *where: the file opened.
inline int openServiceLog(const char* marker, const char* kept, const char* keptOld, const char* ram,
                          const char** where) {
    if (access(marker, F_OK) != 0) {
        *where = "/dev/null";
        return open("/dev/null", O_WRONLY | O_CLOEXEC);
    }
    struct stat st;
    if (stat(kept, &st) == 0 && st.st_size > kKeptLogMax) {
        rename(kept, keptOld);  // nothing writes into it now: the supervisor that did has ended
    }
    const int fd = open(kept, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
        unlink(ram);  // an older log or link of that name
        if (symlink(kept, ram) != 0) {
            // Only a convenience: the log itself is written.
        }
        *where = kept;
        return fd;
    }
    struct stat link;
    if (lstat(ram, &link) == 0 && S_ISLNK(link.st_mode)) {
        unlink(ram);  // the link to the file that could not be opened
    }
    *where = ram;
    return open(ram, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
}

enum class KeptLogTrim {
    kNotKept,  // fd is not the kept log (the /tmp log, /dev/null): the caller bounds it its own way
    kWithin,   // the kept log, within its limit
    kMoved,    // over it: copied to the old file, then emptied
    kEmptied,  // over it, emptied without a copy: the old file could not be written, or it is the old file itself
};

inline bool sameFile(const struct stat& st, const char* path) {
    struct stat named;
    return stat(path, &named) == 0 && named.st_dev == st.st_dev && named.st_ino == st.st_ino;
}

// The supervisor, every 5 s, on its stdout (fd). A writer whose file was renamed to the old one under it (the app,
// restarted while a supervisor still ran, opened a full log again) is emptied there: still bounded.
inline KeptLogTrim trimKeptLog(int fd, const char* kept, const char* keptOld, off_t max) {
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        return KeptLogTrim::kNotKept;
    }
    const bool current = sameFile(st, kept);
    if (!current && !sameFile(st, keptOld)) {
        return KeptLogTrim::kNotKept;
    }
    if (st.st_size <= max) {
        return KeptLogTrim::kWithin;
    }
    if (!current) {
        return ftruncate(fd, 0) == 0 ? KeptLogTrim::kEmptied : KeptLogTrim::kWithin;
    }
    const int in = open(kept, O_RDONLY | O_CLOEXEC);
    const bool copied = in >= 0 && copyToFile(in, keptOld);
    if (in >= 0) {
        close(in);
    }
    if (ftruncate(fd, 0) != 0) {
        return KeptLogTrim::kWithin;  // not emptied: tried again at the next check
    }
    return copied ? KeptLogTrim::kMoved : KeptLogTrim::kEmptied;
}

}  // namespace evo

#endif  // ENTRY_AAD_PLATFORM_KEPT_LOG_HPP
