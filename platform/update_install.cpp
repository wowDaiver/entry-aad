// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The update from a USB stick: the engine, on whatever stands for the car (update_install.hpp).
#include "update_install.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <set>

namespace evo {

namespace {

const long long kSpareBytes = 1024 * 1024;  // left free on the system partition once the new files are written
const size_t kArchivesPerStick = 16;        // archives read on one stick, at most

std::string number(long long value) {
    char text[24];
    snprintf(text, sizeof(text), "%lld", value);
    return text;
}

std::string lower(const std::string& s) {
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = static_cast<char>(out[i] - 'A' + 'a');
        }
    }
    return out;
}

bool regularFile(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool readFile(const std::string& path, size_t max, std::string& out) {
    out.clear();
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return false;
    }
    char buf[65536];
    bool ok = true;
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        if (out.size() + static_cast<size_t>(n) > max) {
            ok = false;
            break;
        }
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return ok;
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

bool syncFolder(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd < 0) {
        return false;
    }
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

std::string folderOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : (slash == 0 ? std::string("/") : path.substr(0, slash));
}

std::string baseName(const std::string& path) {
    return path.substr(path.rfind('/') + 1);
}

// A file that did not exist: created, written, its mode set, flushed. Removed again on failure, errno kept. Its pages
// are then dropped from memory, so that whatever reads it next reads what the flash holds.
bool createFile(const std::string& path, const std::string& content, unsigned mode) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    bool ok =
        writeAll(fd, content.data(), content.size()) && fchmod(fd, static_cast<mode_t>(mode)) == 0 && fsync(fd) == 0;
    if (ok) {
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);  // advice only: a kernel that ignores it reads from memory
    }
    int error = ok ? 0 : errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        error = errno;
    }
    if (!ok) {
        unlink(path.c_str());
        errno = error;
    }
    return ok;
}

// path replaced at once by content, through path.tmp (the journal and the report, on the data partition).
bool replaceFile(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    unlink(tmp.c_str());
    if (!createFile(tmp, content, 0644)) {
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    syncFolder(folderOf(path));
    return true;
}

bool listFolder(const std::string& path, std::vector<std::string>& names) {
    names.clear();
    DIR* folder = opendir(path.c_str());
    if (!folder) {
        return false;
    }
    while (struct dirent* e = readdir(folder)) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            names.push_back(e->d_name);
        }
    }
    closedir(folder);
    std::sort(names.begin(), names.end());
    return true;
}

// The file at path is f exactly: a regular file with its mode, size and SHA-256.
bool holds(const std::string& path, const UpdateFile& f) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 07777) != f.mode ||
        st.st_size != static_cast<off_t>(f.size)) {
        return false;
    }
    std::string content;
    return readFile(path, kUpdateFileMax, content) && sha256Hex(content) == f.sha256;
}

bool isProgram(const std::string& content) {
    return content.size() >= 4 && content.compare(0, 4, "\177ELF") == 0;
}

std::string toHex(const std::string& data) {
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (size_t i = 0; i < data.size(); ++i) {
        hex += kHex[static_cast<unsigned char>(data[i]) >> 4];
        hex += kHex[static_cast<unsigned char>(data[i]) & 15];
    }
    return hex;
}

bool fromHex(const std::string& hex, std::string& data) {
    data.clear();
    if (hex.size() % 2) {
        return false;
    }
    for (size_t i = 0; i < hex.size(); i += 2) {
        int byte = 0;
        for (size_t k = i; k < i + 2; ++k) {
            const char c = hex[k];
            const int digit = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1);
            if (digit < 0) {
                return false;
            }
            byte = byte * 16 + digit;
        }
        data += static_cast<char>(byte);
    }
    return true;
}

// The lines of text, without their end of line.
std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

}  // namespace

bool loaderFoundEverything(const std::string& output, int exitCode, std::string& why) {
    // What the car's loader (glibc 2.15) prints for something missing; then its own errors, which end it before it
    // lists anything ("error while loading shared libraries: ...: wrong ELF class", exit 127).
    static const char* const kMissing[] = {"not found", "undefined symbol", "not defined in file",
                                           "error while loading"};
    const std::vector<std::string> lines = linesOf(output);
    for (size_t l = 0; l < lines.size(); ++l) {
        for (size_t i = 0; i < sizeof(kMissing) / sizeof(kMissing[0]); ++i) {
            if (lines[l].find(kMissing[i]) != std::string::npos) {
                const size_t text = lines[l].find_first_not_of(" \t");
                why = lines[l].substr(text == std::string::npos ? 0 : text, 200);
                return false;
            }
        }
    }
    if (exitCode != 0) {
        char code[48];
        snprintf(code, sizeof(code), "the loader ended with %d", exitCode);
        why = code;
        for (size_t l = 0; l < lines.size(); ++l) {
            if (lines[l].find_first_not_of(" \t") != std::string::npos) {
                why += ": " + lines[l].substr(lines[l].find_first_not_of(" \t"), 200);
                break;
            }
        }
        return false;
    }
    return true;
}

bool systemPartitionWritable(const std::string& mounts) {
    bool writable = false;
    const std::vector<std::string> lines = linesOf(mounts);
    for (size_t l = 0; l < lines.size(); ++l) {
        char device[256], point[256], type[64], options[512];
        if (sscanf(lines[l].c_str(), "%255s %255s %63s %511s", device, point, type, options) == 4 &&
            !strcmp(point, "/")) {
            writable = !strcmp(options, "rw") || !strncmp(options, "rw,", 3);
        }
    }
    return writable;
}

int lockOwnFile(const std::string& path, unsigned waitMs) {
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    for (unsigned waited = 0;; waited += 100) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            return fd;
        }
        if (waited >= waitMs) {
            close(fd);
            return -1;
        }
        usleep(100 * 1000);
    }
}

const char* updateResultText(UpdateResult result) {
    switch (result) {
        case UpdateResult::kInstalled:
            return "installed and checked, the unit restarts";
        case UpdateResult::kNoStick:
            return "no USB stick";
        case UpdateResult::kNewer:
            return "a newer version is ready to install";
        case UpdateResult::kOlder:
            return "an older version is ready to install";
        case UpdateResult::kRepair:
            return "this version is ready to install again: the car's files differ from it";
        case UpdateResult::kPending:
            return "an interrupted update is ready to finish";
        case UpdateResult::kUpToDate:
            return "already up to date";
        case UpdateResult::kNoPackage:
            return "no update archive at the root of the stick";
        case UpdateResult::kInvalid:
            return "no archive both intact and signed by the project";
        case UpdateResult::kIncompatible:
            return "the new programs would not run on this car";
        case UpdateResult::kBusy:
            return "Android Auto or another update is running";
        case UpdateResult::kNotInstalled:
            return "not an installation by this project: nothing touched";
        case UpdateResult::kFailed:
            return "failed, nothing changed";
        case UpdateResult::kIncomplete:
            return "incomplete: install the update again from the stick";
    }
    return "?";
}

Updater::Updater(UpdateHost& host, const UpdatePaths& paths, const std::vector<std::string>& keys,
                 const std::string& installedVersion)
    : host_(host), paths_(paths), keys_(keys), installedVersion_(installedVersion), madeWritable_(false) {}

UpdateResult Updater::check() {
    return run(false);
}

UpdateResult Updater::install() {
    return run(true);
}

void Updater::note(const std::string& line) {
    report_ += line;
    report_ += '\n';
}

UpdateResult Updater::run(bool install) {
    report_.clear();
    package_ = UpdatePackage();
    packageStick_.clear();
    sticks_.clear();
    madeWritable_ = false;
    note("EntryNav2 Android Auto, update from a USB stick, " + host_.stamp());
    note(std::string(install ? "install" : "check") + ", installed version " + installedVersion_);
    const UpdateResult result = install ? installNow() : checkNow();
    note(std::string("result: ") + updateResultText(result) +
         (package_.list.version.empty() ? std::string() : " (version " + package_.list.version + ")"));
    writeReports();
    if (result == UpdateResult::kInstalled) {
        host_.restartUnit();
    }
    return result;
}

bool Updater::ourInstallation() {
    const std::string marker = std::string("/") + kInstalledMarker;
    if (regularFile(onSystem(kUpdateProgramDir + marker)) && regularFile(onSystem(kUpdatePluginDir + marker))) {
        return true;
    }
    note(std::string("no ") + kInstalledMarker + " in /" + kUpdateProgramDir + " and /" + kUpdatePluginDir +
         ": these folders were not installed by this project");
    return false;
}

Updater::Found Updater::findPackage(const std::string& want) {
    sticks_ = usbStickMounts(host_.mounts(), paths_.mediaPrefix);
    if (sticks_.empty()) {
        note("no USB stick mounted under " + paths_.mediaPrefix);
        return kFoundNoStick;
    }
    bool seen = false, valid = false;
    for (size_t s = 0; s < sticks_.size(); ++s) {
        const std::string& stick = sticks_[s].point;
        std::vector<std::string> names;
        if (!listFolder(stick, names)) {
            note("stick " + stick + ": cannot be read");
            continue;
        }
        size_t read = 0;
        for (size_t i = 0; i < names.size(); ++i) {
            const std::string name = lower(names[i]);
            if (name.compare(0, strlen(kUpdateTopPrefix), kUpdateTopPrefix) != 0 || name.size() < 4 ||
                name.compare(name.size() - 4, 4, ".tar") != 0) {
                continue;
            }
            seen = true;
            if (++read > kArchivesPerStick) {
                note("stick " + stick + ": more than " + number(kArchivesPerStick) + " archives, the others left");
                break;
            }
            const std::string path = stick + "/" + names[i];
            struct stat st;
            std::string archive, why;
            if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                note(path + ": not a file");
                continue;
            }
            if (static_cast<unsigned long long>(st.st_size) > kUpdatePackageMax) {
                note(path + ": too large for an update archive");
                continue;
            }
            if (!readFile(path, kUpdatePackageMax, archive)) {
                note(path + ": cannot be read from the stick");
                continue;
            }
            UpdatePackage candidate;
            if (!readUpdatePackage(archive, keys_, candidate, why)) {
                note(path + ": not taken: " + why);
                continue;
            }
            note(path + ": version " + candidate.list.version + ", signed by the project, " +
                 number(static_cast<long long>(candidate.list.files.size())) + " files intact");
            valid = true;
            // The install takes the archive the check offered and no other; else the newest.
            const bool wanted = want.empty() ? package_.list.version.empty() ||
                                                   compareVersions(candidate.list.version, package_.list.version) > 0
                                             : package_.list.version.empty() && sha256Hex(candidate.listText) == want;
            if (wanted) {
                std::swap(package_, candidate);
                packageStick_ = stick;
            }
        }
    }
    if (!package_.list.version.empty()) {
        note("chosen: version " + package_.list.version + ", on " + packageStick_);
        return kFoundValid;
    }
    if (valid) {
        note("the stick no longer holds the archive the check found: check again");
        return kFoundOther;
    }
    if (!seen) {
        note("no entrynav2-android-auto-<version>-armhf.tar at the root of the stick");
        return kFoundNothing;
    }
    return kFoundInvalid;
}

Updater::Plan Updater::plan(const UpdateList& list) {
    Plan p;
    if (list.files.empty()) {  // a list always names the programs (parseUpdateList): never "remove everything"
        note("no list of files to compare the car with");
        return p;
    }
    std::set<std::string> listed;
    for (size_t i = 0; i < list.files.size(); ++i) {
        const UpdateFile& f = list.files[i];
        listed.insert(f.path);
        if (!holds(onSystem(f.path), f)) {
            p.changed.push_back(i);
            p.bytes += static_cast<long long>(f.size);
        }
    }
    const char* const folders[] = {kUpdateProgramDir, kUpdatePluginDir};
    for (size_t d = 0; d < 2; ++d) {
        std::vector<std::string> names;
        listFolder(onSystem(folders[d]), names);
        for (size_t i = 0; i < names.size(); ++i) {
            const std::string path = std::string(folders[d]) + "/" + names[i];
            struct stat st;
            if (names[i] == kInstalledMarker || listed.count(path) || lstat(onSystem(path).c_str(), &st) != 0) {
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                note("left in place: the folder /" + path);
                continue;
            }
            p.extra.push_back(path);
        }
    }
    note(number(static_cast<long long>(p.changed.size())) + " of " + number(static_cast<long long>(list.files.size())) +
         " files differ from the car's, " + number(static_cast<long long>(p.extra.size())) + " to remove");
    return p;
}

// 1: every program fits the car and loads, 0: one does not, -1: they could not be given to the loader.
int Updater::programsLoad() {
    int fits = 1;
    for (size_t i = 0; i < package_.list.files.size(); ++i) {
        const UpdateFile& f = package_.list.files[i];
        const bool library = f.path.size() > 3 && f.path.compare(f.path.size() - 3, 3, ".so") == 0;
        std::string why;
        // Every executable file of the list is a program the car runs; any other ELF file is checked too.
        if ((f.mode == 0755 || isProgram(package_.contents[i])) &&
            !programFitsCar(package_.contents[i], library, why)) {
            note("/" + f.path + " is not for this car: " + why);
            fits = 0;
        }
    }
    if (!fits) {
        return 0;
    }
    const std::string& staging = paths_.staging;
    std::vector<std::string> stale;
    if (mkdir(staging.c_str(), 0700) != 0 && errno != EEXIST) {
        note("cannot create " + staging + " for the loader");
        return -1;
    }
    struct stat st;
    if (lstat(staging.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        note(staging + " is not a folder");
        return -1;
    }
    listFolder(staging, stale);
    for (size_t i = 0; i < stale.size(); ++i) {
        unlink((staging + "/" + stale[i]).c_str());
    }
    int result = 1;
    std::vector<std::string> staged;
    for (size_t i = 0; i < package_.list.files.size() && result >= 0; ++i) {
        if (!isProgram(package_.contents[i])) {
            continue;
        }
        const std::string& path = package_.list.files[i].path;
        const std::string copy = staging + "/" + baseName(path);
        if (!createFile(copy, package_.contents[i], 0755)) {
            note("cannot put " + baseName(path) + " in " + staging + " for the loader: " + strerror(errno));
            result = -1;
            break;
        }
        staged.push_back(copy);
        std::string why;
        if (host_.loads(copy, why)) {
            note("the car's loader finds everything /" + path + " needs");
        } else {
            note("the car's loader refuses /" + path + ": " + why);
            result = 0;
        }
    }
    for (size_t i = 0; i < staged.size(); ++i) {
        unlink(staged[i].c_str());
    }
    rmdir(staging.c_str());
    return result;
}

bool Updater::beginWritable() {
    if (host_.systemWritable()) {
        note("the system partition was already writable: left so");
        return true;
    }
    if (!host_.setSystemWritable(true)) {
        note("the system partition refused to become writable");
        return false;
    }
    madeWritable_ = true;
    note("system partition writable");
    return true;
}

void Updater::endWritable() {
    if (!madeWritable_) {
        return;
    }
    madeWritable_ = false;
    sync();
    if (host_.setSystemWritable(false)) {
        note("system partition read-only again");
    } else {
        note(
            "system partition still writable: the iDrive still uses a replaced file; the restart puts it back "
            "read-only");
    }
}

bool Updater::writeNewFiles(const Plan& plan) {
    std::vector<std::string> written;
    bool ok = true;
    for (size_t k = 0; k < plan.changed.size() && ok; ++k) {
        const size_t i = plan.changed[k];
        const UpdateFile& f = package_.list.files[i];
        const std::string next = onSystem(f.path) + ".new";
        struct stat st;
        if (lstat(next.c_str(), &st) == 0 && (S_ISDIR(st.st_mode) || unlink(next.c_str()) != 0)) {
            note("cannot write /" + f.path + ".new: something else is in its place");
            ok = false;
        } else if (!createFile(next, package_.contents[i], f.mode)) {
            note("cannot write /" + f.path + ".new: " + strerror(errno));
            ok = false;
        } else {
            written.push_back(next);
            if (!holds(next, f)) {
                note("/" + f.path + ".new differs from the archive once written");
                ok = false;
            }
        }
    }
    if (!ok) {
        for (size_t k = 0; k < written.size(); ++k) {
            unlink(written[k].c_str());
        }
    }
    syncFolder(onSystem(kUpdateProgramDir));
    syncFolder(onSystem(kUpdatePluginDir));
    if (ok) {
        note(number(static_cast<long long>(written.size())) + " new files written next to the old ones and checked");
    }
    return ok;
}

// The journal: one file, replaced at once, the signature in hex on its first line, then the signed list. A journal
// that cannot be written leaves the one before it as it was.
bool Updater::writePending() {
    if (replaceFile(paths_.pending, toHex(package_.signature) + "\n" + package_.listText)) {
        note("journal written: from here the update can always be finished");
        return true;
    }
    note("the journal cannot be written on the data partition");
    return false;
}

bool Updater::swap(const UpdateList& list, const std::vector<size_t>& files) {
    bool ok = true;
    size_t done = 0;
    for (size_t k = 0; k < files.size() && ok; ++k) {
        const std::string target = onSystem(list.files[files[k]].path);
        if (rename((target + ".new").c_str(), target.c_str()) == 0) {
            ++done;
        } else {
            note("cannot put /" + list.files[files[k]].path + " in place: " + strerror(errno));
            ok = false;
        }
    }
    syncFolder(onSystem(kUpdateProgramDir));
    syncFolder(onSystem(kUpdatePluginDir));
    note(number(static_cast<long long>(done)) + " of " + number(static_cast<long long>(files.size())) +
         " files put in place");
    return ok;
}

void Updater::removeExtra(const std::vector<std::string>& extra) {
    for (size_t i = 0; i < extra.size(); ++i) {
        if (unlink(onSystem(extra[i]).c_str()) == 0) {
            note("removed /" + extra[i]);
        } else if (errno != ENOENT) {  // a stale .new the swap has just renamed is gone already
            note("could not remove /" + extra[i] + ": " + strerror(errno));
        }
    }
    if (!extra.empty()) {
        syncFolder(onSystem(kUpdateProgramDir));
        syncFolder(onSystem(kUpdatePluginDir));
    }
}

void Updater::removePending() {
    unlink(paths_.pending.c_str());
    syncFolder(folderOf(paths_.pending));
}

// 0: no journal, 1: a journal signed by the project, -1: one that cannot be trusted.
int Updater::readPending(UpdatePackage& journal) {
    journal = UpdatePackage();
    if (!regularFile(paths_.pending)) {
        return 0;
    }
    std::string why, text;
    const bool read = readFile(paths_.pending, 2048 + 1 + kUpdateListMax, text);
    const size_t line = text.find('\n');
    if (read && line != std::string::npos) {
        journal.listText = text.substr(line + 1);
    }
    if (!read || line == std::string::npos || !fromHex(text.substr(0, line), journal.signature) ||
        !signedByOneOf(journal.listText, journal.signature, keys_)) {
        note("journal of an interrupted update found, but unreadable or not signed by the project");
        return -1;
    }
    if (!parseUpdateList(journal.listText, journal.list, why)) {
        note("journal of an interrupted update found, but unreadable: " + why);
        return -1;
    }
    note("journal of an interrupted update to version " + journal.list.version + " found");
    return 1;
}

bool Updater::pendingFinishable(const UpdatePackage& journal, std::vector<size_t>& toRename) {
    toRename.clear();
    for (size_t i = 0; i < journal.list.files.size(); ++i) {
        const UpdateFile& f = journal.list.files[i];
        if (holds(onSystem(f.path), f)) {
            continue;
        }
        if (holds(onSystem(f.path) + ".new", f)) {
            toRename.push_back(i);
            continue;
        }
        note("the journal cannot be finished: /" + f.path + " is neither the new file nor a checked .new");
        return false;
    }
    note(number(static_cast<long long>(toRename.size())) + " files of the journal still to put in place");
    return true;
}

UpdateResult Updater::finishPending() {
    UpdatePackage journal;
    const int pending = readPending(journal);
    if (pending <= 0) {  // gone meanwhile: nothing to finish, nothing done
        return pending < 0 ? UpdateResult::kIncomplete : UpdateResult::kFailed;
    }
    package_.list = journal.list;  // for the report
    std::vector<size_t> toRename;
    if (!pendingFinishable(journal, toRename) || !beginWritable()) {
        return UpdateResult::kIncomplete;
    }
    const bool swapped = swap(journal.list, toRename);
    if (swapped) {
        removeExtra(plan(journal.list).extra);
        removePending();
    }
    sync();
    endWritable();
    return swapped ? UpdateResult::kInstalled : UpdateResult::kIncomplete;
}

UpdateResult Updater::checkNow() {
    unlink(paths_.checked.c_str());  // set again below if this check offers an archive
    if (!ourInstallation()) {
        return UpdateResult::kNotInstalled;
    }
    const Found found = findPackage(std::string());
    if (found != kFoundValid) {
        UpdatePackage journal;
        const int pending = readPending(journal);
        if (pending) {
            std::vector<size_t> toRename;
            if (pending > 0 && pendingFinishable(journal, toRename)) {
                package_.list = journal.list;
                return UpdateResult::kPending;
            }
            return UpdateResult::kIncomplete;
        }
        return found == kFoundNoStick   ? UpdateResult::kNoStick
               : found == kFoundNothing ? UpdateResult::kNoPackage
                                        : UpdateResult::kInvalid;
    }
    const Plan p = plan(package_.list);
    const bool journal = regularFile(paths_.pending);
    if (p.changed.empty() && p.extra.empty() && !journal) {
        return UpdateResult::kUpToDate;
    }
    const int loads = programsLoad();
    if (loads <= 0) {
        return loads == 0 ? UpdateResult::kIncompatible : UpdateResult::kFailed;
    }
    // What the install will take: this archive, by its signed list.
    if (!replaceFile(paths_.checked, sha256Hex(package_.listText) + "\n")) {
        note("cannot note which archive was checked in " + paths_.checked);
        return UpdateResult::kFailed;
    }
    if (p.changed.empty() && p.extra.empty()) {
        note(
            "the car holds every file of this version, but the journal of an earlier update is left: an install "
            "removes it");
        return UpdateResult::kRepair;
    }
    const int order = compareVersions(package_.list.version, installedVersion_);
    return order > 0 ? UpdateResult::kNewer : (order < 0 ? UpdateResult::kOlder : UpdateResult::kRepair);
}

UpdateResult Updater::installNow() {
    // The archive the last check offered, if a check came first (the menu always checks first): no other is taken.
    std::string checked;
    readFile(paths_.checked, 128, checked);
    unlink(paths_.checked.c_str());
    checked = checked.substr(0, checked.find('\n'));
    if (!ourInstallation()) {
        return UpdateResult::kNotInstalled;
    }
    const Found found = findPackage(checked);
    if (found == kFoundOther) {
        return UpdateResult::kFailed;
    }
    if (found != kFoundValid) {
        if (regularFile(paths_.pending)) {
            return finishPending();
        }
        return found == kFoundNoStick   ? UpdateResult::kNoStick
               : found == kFoundNothing ? UpdateResult::kNoPackage
                                        : UpdateResult::kInvalid;
    }
    const Plan p = plan(package_.list);
    if (p.changed.empty() && p.extra.empty()) {
        if (regularFile(paths_.pending)) {
            removePending();
            note("journal of an earlier update removed: the car already holds every file of this version");
        }
        return UpdateResult::kUpToDate;
    }
    const int loads = programsLoad();
    if (loads <= 0) {
        return loads == 0 ? UpdateResult::kIncompatible : UpdateResult::kFailed;
    }
    const long long free = host_.systemFreeBytes();
    if (free < p.bytes + kSpareBytes) {
        note("not enough room on the system partition: " + number(free / 1024) + " KB free, " +
             number((p.bytes + kSpareBytes) / 1024) + " KB needed");
        return UpdateResult::kFailed;
    }
    if (!beginWritable()) {
        return UpdateResult::kFailed;
    }
    if (!writeNewFiles(p) || !writePending()) {
        for (size_t k = 0; k < p.changed.size(); ++k) {
            unlink((onSystem(package_.list.files[p.changed[k]].path) + ".new").c_str());
        }
        endWritable();
        return UpdateResult::kFailed;
    }
    if (!swap(package_.list, p.changed)) {
        sync();
        endWritable();
        return UpdateResult::kIncomplete;
    }
    removeExtra(p.extra);
    removePending();
    sync();
    endWritable();
    return UpdateResult::kInstalled;
}

void Updater::writeReports() {
    replaceFile(paths_.report, report_);
    // On the stick: the one the archive came from when it can be written, else the first that can.
    std::string stick;
    for (size_t i = 0; i < sticks_.size(); ++i) {
        if (sticks_[i].writable && (stick.empty() || sticks_[i].point == packageStick_)) {
            stick = sticks_[i].point;
        }
    }
    if (stick.empty()) {
        return;
    }
    const int fd = open((stick + "/" + kUpdateStickReport).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        if (writeAll(fd, report_.data(), report_.size())) {
            fsync(fd);
        }
        close(fd);
    }
}

}  // namespace evo
