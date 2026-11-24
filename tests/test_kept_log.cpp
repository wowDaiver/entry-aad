// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The service's log kept on the data partition (platform/kept_log.hpp), in a temporary folder: where it is opened,
// the /tmp link, the old part at opening, the trim while a writer keeps its descriptor, the fallback to /tmp.
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "../platform/kept_log.hpp"

namespace {

std::string g_dir;

std::string at(const char* name) {
    return g_dir + "/" + name;
}

off_t sizeOf(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? st.st_size : -1;
}

void writeBytes(int fd, size_t n, char c) {
    std::string block(n, c);
    assert(write(fd, block.data(), block.size()) == static_cast<ssize_t>(block.size()));
}

void touch(const std::string& path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0644);
    assert(fd >= 0);
    close(fd);
}

}  // namespace

int main() {
    char dir[] = "/tmp/entry-aad-kept-log-XXXXXX";
    assert(mkdtemp(dir));
    g_dir = dir;
    assert(mkdir(at("data").c_str(), 0755) == 0);
    const std::string marker = at("data/maintenance"), kept = at("data/service.log"), old = at("data/service.old.log"),
                      ram = at("entry-aa-service.log");
    const char* where = nullptr;

    // Production: nothing written, anywhere.
    int fd = evo::openServiceLog(marker.c_str(), kept.c_str(), old.c_str(), ram.c_str(), &where);
    assert(fd >= 0 && !strcmp(where, "/dev/null") && sizeOf(kept) < 0 && sizeOf(ram) < 0);
    close(fd);

    // Maintenance: the data partition, and the /tmp name a link to it.
    touch(marker);
    fd = evo::openServiceLog(marker.c_str(), kept.c_str(), old.c_str(), ram.c_str(), &where);
    assert(fd >= 0 && kept == where);
    writeBytes(fd, 100, 'a');
    close(fd);
    struct stat link;
    assert(lstat(ram.c_str(), &link) == 0 && S_ISLNK(link.st_mode) && sizeOf(ram) == 100);

    // A full log at the next start becomes the old part; the new start begins empty.
    fd = open(kept.c_str(), O_WRONLY | O_APPEND);
    writeBytes(fd, static_cast<size_t>(evo::kKeptLogMax), 'b');
    close(fd);
    fd = evo::openServiceLog(marker.c_str(), kept.c_str(), old.c_str(), ram.c_str(), &where);
    assert(fd >= 0 && kept == where && sizeOf(kept) == 0 && sizeOf(old) == evo::kKeptLogMax + 100);

    // While it runs: within the limit nothing moves; over it, the content goes to the old part and the log empties,
    // and the writer's next line, through the same descriptor, starts the emptied file.
    writeBytes(fd, 1000, 'c');
    assert(evo::trimKeptLog(fd, kept.c_str(), old.c_str(), 4096) == evo::KeptLogTrim::kWithin);
    writeBytes(fd, 5000, 'd');
    assert(evo::trimKeptLog(fd, kept.c_str(), old.c_str(), 4096) == evo::KeptLogTrim::kMoved);
    assert(sizeOf(kept) == 0 && sizeOf(old) == 6000);
    writeBytes(fd, 10, 'e');
    assert(sizeOf(kept) == 10);
    // The old part cannot be written: emptied all the same, the partition must not fill.
    assert(rename(old.c_str(), at("moved-away").c_str()) == 0 && mkdir(old.c_str(), 0755) == 0);
    writeBytes(fd, 5000, 'f');
    assert(evo::trimKeptLog(fd, kept.c_str(), old.c_str(), 4096) == evo::KeptLogTrim::kEmptied && sizeOf(kept) == 0);
    assert(rmdir(old.c_str()) == 0);
    // Renamed to the old part under a writer that still runs (the app restarted and opened a full log again): that
    // writer is still bounded, emptied where its file now is.
    assert(rename(kept.c_str(), old.c_str()) == 0);
    writeBytes(fd, 5000, 'f');
    assert(evo::trimKeptLog(fd, kept.c_str(), old.c_str(), 4096) == evo::KeptLogTrim::kEmptied && sizeOf(old) == 0);
    close(fd);

    // Something else than the kept log (the /tmp log, /dev/null): left to the caller.
    const int other = open(at("other.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    writeBytes(other, 5000, 'g');
    assert(evo::trimKeptLog(other, kept.c_str(), old.c_str(), 4096) == evo::KeptLogTrim::kNotKept);
    close(other);
    const int null = open("/dev/null", O_WRONLY);
    assert(evo::trimKeptLog(null, kept.c_str(), old.c_str(), 4096) == evo::KeptLogTrim::kNotKept);
    close(null);

    // The data partition refuses it: /tmp, as before, a real file in place of the link.
    const std::string missing = at("no-such-dir/service.log"), missingOld = at("no-such-dir/service.old.log");
    fd = evo::openServiceLog(marker.c_str(), missing.c_str(), missingOld.c_str(), ram.c_str(), &where);
    assert(fd >= 0 && ram == where);
    writeBytes(fd, 7, 'h');
    close(fd);
    assert(lstat(ram.c_str(), &link) == 0 && S_ISREG(link.st_mode) && sizeOf(ram) == 7);

    const std::string clean = "rm -rf '" + g_dir + "'";
    assert(system(clean.c_str()) == 0);
    puts(
        "PASS kept service log: none in production, data partition with a /tmp link, old part at opening, trimmed "
        "under a live writer, emptied when the old part fails, /tmp when the partition refuses");
    return 0;
}
