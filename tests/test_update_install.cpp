// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The update from a USB stick (platform/update_install.hpp) on a car made of temporary folders: the system's two
// folders, the data partition, a stick under media/. A fake host stands for the remounts, the car's loader and the
// restart. Each case checks what changed on the "car", and above all what did not.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <map>
#include <sstream>

#include "../platform/update_install.hpp"
#include "update_test_kit.hpp"

namespace {

std::string g_base;  // the test's folder

std::string read(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    std::stringstream all;
    all << in.rdbuf();
    return all.str();
}

void put(const std::string& path, const std::string& text, unsigned mode = 0644) {
    unlink(path.c_str());
    std::ofstream out(path.c_str(), std::ios::binary);
    out << text;
    out.close();
    chmod(path.c_str(), mode);
}

// The journal holds its signature in hex on its first line.
std::string hex(const std::string& data) {
    std::string out;
    for (size_t i = 0; i < data.size(); ++i) {
        char two[3];
        snprintf(two, sizeof(two), "%02x", static_cast<unsigned char>(data[i]));
        out += two;
    }
    return out;
}

bool exists(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

unsigned modeOf(const std::string& path) {
    struct stat st;
    CHECK(stat(path.c_str(), &st) == 0);
    return st.st_mode & 07777;
}

void removeTree(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        while (struct dirent* e = d ? readdir(d) : nullptr) {
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                removeTree(path + "/" + e->d_name);
            }
        }
        if (d) {
            closedir(d);
        }
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}

// Every file under path with its content and mode: what a case compares before and after.
void snapshot(const std::string& path, const std::string& name, std::map<std::string, std::string>& out) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        out[name + "/"] = "folder";
        DIR* d = opendir(path.c_str());
        while (struct dirent* e = d ? readdir(d) : nullptr) {
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                snapshot(path + "/" + e->d_name, name + "/" + e->d_name, out);
            }
        }
        if (d) {
            closedir(d);
        }
        return;
    }
    char mode[16];
    snprintf(mode, sizeof(mode), "%o:", st.st_mode & 07777);
    out[name] = mode + read(path);
}

class FakeCar : public evo::UpdateHost {
public:
    FakeCar() : writable(false), refuseRw(false), refuseRo(false), free(1LL << 30), remounts(0), restarts(0) {}
    std::string mounts() override { return mountsText; }
    bool systemWritable() override { return writable; }
    bool setSystemWritable(bool w) override {
        ++remounts;
        if ((w && !refuseRw) || (!w && !refuseRo)) {
            writable = w;
        }
        return writable == w;
    }
    bool loads(const std::string& path, std::string& why) override {
        CHECK(path.compare(0, staging.size(), staging) == 0 && exists(path));  // only from the folder in memory
        loaded.push_back(path.substr(path.rfind('/') + 1));
        if (refused.count(loaded.back())) {
            why = "libmissing.so => not found";
            return false;
        }
        return true;
    }
    long long systemFreeBytes() override { return free; }
    void restartUnit() override { ++restarts; }
    std::string stamp() override { return "2026-11-24 10:00:00"; }

    std::string mountsText, staging;
    bool writable, refuseRw, refuseRo;
    long long free;
    int remounts, restarts;
    std::map<std::string, bool> refused;
    std::vector<std::string> loaded;
};

struct Car {
    std::string root, data, stick;
    evo::UpdatePaths paths;
    FakeCar host;
    std::vector<std::string> keys;
};

void installRelease(const std::string& root, const kit::Release& r) {
    for (size_t i = 0; i < r.files.size(); ++i) {
        put(root + "/" + r.files[i].path, r.files[i].content, r.files[i].mode);
    }
}

// A car running version installed, with a stick (writable unless told), and the test's key trusted.
void makeCar(Car& car, EVP_PKEY* key, const std::string& installed, bool stickWritable = true) {
    removeTree(g_base);
    CHECK(mkdir(g_base.c_str(), 0700) == 0);
    car.root = g_base + "/root";
    car.data = g_base + "/data";
    car.stick = g_base + "/media/sda1";
    const char* const folders[] = {"/root",
                                   "/root/opt",
                                   "/root/opt/aa",
                                   "/root/opt/hmi",
                                   "/root/opt/hmi/data",
                                   "/root/opt/hmi/data/ro",
                                   "/root/opt/hmi/data/ro/Plugins",
                                   "/root/opt/hmi/data/ro/Plugins/EntryAad",
                                   "/data",
                                   "/media",
                                   "/media/sda1"};
    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); ++i) {
        CHECK(mkdir((g_base + folders[i]).c_str(), 0755) == 0);
    }
    installRelease(car.root, kit::release(installed));
    put(car.root + "/opt/aa/.installed-by-entryaad", "");
    put(car.root + "/opt/hmi/data/ro/Plugins/EntryAad/.installed-by-entryaad", "");
    car.paths.root = car.root;
    car.paths.pending = car.data + "/update-pending";
    car.paths.checked = g_base + "/update.checked";
    car.paths.report = car.data + "/update-report.txt";
    car.paths.staging = g_base + "/staging";
    car.paths.mediaPrefix = g_base + "/media/";
    car.host = FakeCar();
    car.host.staging = car.paths.staging;
    car.host.mountsText = "/dev/root / ext3 ro,relatime 0 0\n/dev/mmcblk0p9 /var/local ext4 rw,noexec 0 0\n/dev/sda1 " +
                          car.stick + " tfat " + (stickWritable ? "rw" : "ro") + ",noexec,gid=1004,umask=002 0 0\n";
    car.keys.assign(1, kit::publicDer(key));
}

evo::UpdateResult check(Car& car, const std::string& installed) {
    evo::Updater updater(car.host, car.paths, car.keys, installed);
    return updater.check();
}

evo::UpdateResult install(Car& car, const std::string& installed, std::string* report = nullptr) {
    evo::Updater updater(car.host, car.paths, car.keys, installed);
    const evo::UpdateResult r = updater.install();
    if (report) {
        *report = updater.report();
    }
    return r;
}

std::map<std::string, std::string> carFiles(const Car& car) {
    std::map<std::string, std::string> files;
    snapshot(car.root, "", files);
    snapshot(car.data, "data", files);
    files.erase("data/update-report.txt");  // written by every run
    return files;
}

bool holdsRelease(const Car& car, const kit::Release& r) {
    for (size_t i = 0; i < r.files.size(); ++i) {
        const std::string path = car.root + "/" + r.files[i].path;
        if (read(path) != r.files[i].content || modeOf(path) != r.files[i].mode) {
            return false;
        }
    }
    return true;
}

bool noNewFiles(const Car& car) {
    std::map<std::string, std::string> files;
    snapshot(car.root, "", files);
    for (std::map<std::string, std::string>::const_iterator i = files.begin(); i != files.end(); ++i) {
        if (i->first.size() > 4 && i->first.compare(i->first.size() - 4, 4, ".new") == 0) {
            return false;
        }
    }
    return true;
}

void putArchive(const Car& car, const std::string& name, const std::string& archive) {
    put(car.stick + "/" + name, archive);
}

std::string archiveName(const std::string& version) {
    return "entrynav2-android-auto-" + version + "-armhf.tar";
}

// ---- check: nothing changes -----------------------------------------------------------------------------------------
void checks(EVP_PKEY* key, EVP_PKEY* stranger) {
    Car car;
    makeCar(car, key, "0.13.0");
    const std::map<std::string, std::string> before = carFiles(car);
    car.host.mountsText = "/dev/root / ext3 ro 0 0\n";
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kNoStick);
    makeCar(car, key, "0.13.0");
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kNoPackage);
    putArchive(car, "notes.txt", "x");
    putArchive(car, "entrynav2-android-auto-0.14.0-armhf.tar.sha256", "x");
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kNoPackage);
    putArchive(car, archiveName("0.14.0"), kit::archive(kit::release("0.14.0"), stranger));
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kInvalid);
    CHECK(read(car.stick + "/EntryAA-update.txt").find("not taken: the signature is not the project's") !=
          std::string::npos);
    // Newer, signed: ready; the stick's name may differ (a browser's " (1)"), its case too.
    putArchive(car, "EntryNav2-Android-Auto-0.14.0-armhf (1).TAR", kit::archive(kit::release("0.14.0"), key));
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kNewer);
    CHECK(car.host.loaded.size() == 3);  // the supervisor, the service and the app went through the car's loader
    CHECK(!exists(car.paths.staging));
    const std::string report = read(car.paths.report);
    CHECK(report.find("chosen: version 0.14.0") != std::string::npos);
    CHECK(report.find("result: a newer version is ready to install (version 0.14.0)") != std::string::npos);
    CHECK(read(car.stick + "/EntryAA-update.txt") == report);
    CHECK(carFiles(car) == before);
    CHECK(car.host.remounts == 0 && car.host.restarts == 0);

    // Older, the same version, the same files.
    makeCar(car, key, "0.13.0");
    putArchive(car, archiveName("0.12.1"), kit::archive(kit::release("0.12.1"), key));
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kOlder);
    makeCar(car, key, "0.13.0");
    putArchive(car, archiveName("0.13.0"), kit::archive(kit::release("0.13.0"), key));
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kUpToDate);
    put(car.root + "/opt/aa/entry-aad", "damaged", 0755);
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kRepair);
    makeCar(car, key, "0.13.0");
    putArchive(car, archiveName("0.13.0"), kit::archive(kit::release("0.13.0"), key));
    chmod((car.root + "/opt/aa/entry-aad").c_str(), 0644);  // not executable any more
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kRepair);
    makeCar(car, key, "0.13.0");
    putArchive(car, archiveName("0.13.0"), kit::archive(kit::release("0.13.0"), key));
    put(car.root + "/opt/aa/aa-supervisor.new", "left by a power cut");
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kRepair);  // the stray file is removed by an install

    // Of several archives, the newest valid one.
    makeCar(car, key, "0.13.0");
    putArchive(car, archiveName("0.13.1"), kit::archive(kit::release("0.13.1"), key));
    putArchive(car, archiveName("0.15.0"), kit::archive(kit::release("0.15.0"), stranger));
    putArchive(car, archiveName("0.14.0-rc.2"), kit::archive(kit::release("0.14.0-rc.2"), key));
    putArchive(car, archiveName("0.14.0-rc.10"), kit::archive(kit::release("0.14.0-rc.10"), key));
    evo::Updater updater(car.host, car.paths, car.keys, "0.13.0");
    CHECK(updater.check() == evo::UpdateResult::kNewer && updater.foundVersion() == "0.14.0-rc.10");

    // A program the car's loader refuses.
    makeCar(car, key, "0.13.0");
    putArchive(car, archiveName("0.14.0"), kit::archive(kit::release("0.14.0"), key));
    car.host.refused["entry-aad"] = true;
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kIncompatible);
    CHECK(read(car.paths.report).find("the car's loader refuses /opt/aa/entry-aad: libmissing.so => not found") !=
          std::string::npos);
    CHECK(!exists(car.paths.staging));

    // Not our installation: nothing is read further.
    makeCar(car, key, "0.13.0");
    putArchive(car, archiveName("0.14.0"), kit::archive(kit::release("0.14.0"), key));
    unlink((car.root + "/opt/hmi/data/ro/Plugins/EntryAad/.installed-by-entryaad").c_str());
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kNotInstalled);
    CHECK(install(car, "0.13.0") == evo::UpdateResult::kNotInstalled && car.host.remounts == 0);

    // A read-only stick: the report stays on the car only.
    makeCar(car, key, "0.13.0", false);
    putArchive(car, archiveName("0.14.0"), kit::archive(kit::release("0.14.0"), key));
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kNewer);
    CHECK(!exists(car.stick + "/EntryAA-update.txt") && exists(car.paths.report));
    printf("PASS check\n");
}

// ---- install ------------------------------------------------------------------------------------------------------
void installs(EVP_PKEY* key) {
    Car car;
    makeCar(car, key, "0.13.0");
    const kit::Release next = kit::release("0.14.0");
    putArchive(car, archiveName("0.14.0"), kit::archive(next, key));
    put(car.root + "/opt/aa/evo-trial", "a program the new version no longer has", 0755);
    put(car.data + "/autostart", "1\n");
    std::string report;
    CHECK(install(car, "0.13.0", &report) == evo::UpdateResult::kInstalled);
    CHECK(holdsRelease(car, next));
    CHECK(!exists(car.root + "/opt/aa/evo-trial"));
    CHECK(exists(car.root + "/opt/aa/.installed-by-entryaad"));
    CHECK(exists(car.root + "/opt/hmi/data/ro/Plugins/EntryAad/.installed-by-entryaad"));
    CHECK(read(car.data + "/autostart") == "1\n");  // the settings stay
    CHECK(!exists(car.paths.pending) && noNewFiles(car));
    CHECK(!car.host.writable && car.host.remounts == 2 && car.host.restarts == 1);
    CHECK(report.find("removed /opt/aa/evo-trial") != std::string::npos);
    CHECK(report.find("result: installed and checked, the unit restarts (version 0.14.0)") != std::string::npos);
    CHECK(read(car.stick + "/EntryAA-update.txt") == report);
    // Again: nothing to do, nothing remounted, no restart.
    CHECK(install(car, "0.14.0") == evo::UpdateResult::kUpToDate);
    CHECK(car.host.remounts == 2 && car.host.restarts == 1);

    // An older version, chosen by the driver: the same way.
    makeCar(car, key, "0.14.0");
    const kit::Release older = kit::release("0.13.0");
    putArchive(car, archiveName("0.13.0"), kit::archive(older, key));
    CHECK(install(car, "0.14.0") == evo::UpdateResult::kInstalled && holdsRelease(car, older));

    // Only what differs is written; the read-only remount refused (the iDrive keeps the old app open): installed.
    makeCar(car, key, "0.14.0");
    put(car.root + "/opt/hmi/data/ro/Plugins/EntryAad/EntryAad.xml", "changed by hand");
    putArchive(car, archiveName("0.14.0"), kit::archive(next, key));
    car.host.refuseRo = true;
    CHECK(install(car, "0.14.0", &report) == evo::UpdateResult::kInstalled && holdsRelease(car, next));
    CHECK(report.find("1 of 7 files differ") != std::string::npos);
    CHECK(report.find("still writable") != std::string::npos);

    // The system partition already writable: left so, never remounted.
    makeCar(car, key, "0.13.0");
    putArchive(car, archiveName("0.14.0"), kit::archive(next, key));
    car.host.writable = true;
    CHECK(install(car, "0.13.0") == evo::UpdateResult::kInstalled && holdsRelease(car, next));
    CHECK(car.host.writable && car.host.remounts == 0);
    printf("PASS install\n");
}

// ---- refusals: nothing changed -----------------------------------------------------------------------------------
void refusals(EVP_PKEY* key) {
    Car car;
    const kit::Release old = kit::release("0.13.0"), next = kit::release("0.14.0");
    struct Case {
        const char* name;
        evo::UpdateResult expected;
    };
    // Each case: a car on 0.13.0, the stick with 0.14.0, one thing in the way.
    for (int c = 0; c < 6; ++c) {
        makeCar(car, key, "0.13.0");
        putArchive(car, archiveName("0.14.0"), kit::archive(next, key));
        evo::UpdateResult expected = evo::UpdateResult::kFailed;
        switch (c) {
            case 0:
                car.host.free = 3000 + 1024 * 1024;  // less than the new files and the spare MB
                break;
            case 1:
                car.host.refuseRw = true;
                break;
            case 2:  // something that cannot be replaced where a new file goes: found after some files are written
                CHECK(mkdir((car.root + "/opt/hmi/data/ro/Plugins/EntryAad/libEntryAad.so.new").c_str(), 0755) == 0);
                break;
            case 3:  // the journal cannot be written
                CHECK(mkdir(car.paths.pending.c_str(), 0755) == 0);
                break;
            case 4:
                car.host.refused["libEntryAad.so"] = true;
                expected = evo::UpdateResult::kIncompatible;
                break;
            case 5:
                unlink((car.stick + "/" + archiveName("0.14.0")).c_str());
                put(car.stick + "/" + archiveName("0.14.0"), "not an archive");
                expected = evo::UpdateResult::kInvalid;
                break;
        }
        const std::map<std::string, std::string> before = carFiles(car);
        CHECK(install(car, "0.13.0") == expected);
        CHECK(carFiles(car) == before);
        CHECK(holdsRelease(car, old) && noNewFiles(car) && !car.host.writable && car.host.restarts == 0);
        CHECK(exists(car.paths.pending) == (c == 3));  // only the folder of case 3
    }
    printf("PASS refusals\n");
}

// ---- a power cut during the swap: the journal finishes it -----------------------------------------------------------
// The state the car would be in after the journal is written and some renames are done: those files new, the others
// still old with their checked .new beside them.
void cutAfter(Car& car, EVP_PKEY* key, const kit::Release& next, size_t renamed) {
    for (size_t i = 0; i < next.files.size(); ++i) {
        const std::string path = car.root + "/" + next.files[i].path;
        put(i < renamed ? path : path + ".new", next.files[i].content, next.files[i].mode);
    }
    put(car.paths.pending, hex(kit::sign(key, kit::listText(next))) + "\n" + kit::listText(next));
}

void journal(EVP_PKEY* key, EVP_PKEY* stranger) {
    Car car;
    const kit::Release old = kit::release("0.13.0"), next = kit::release("0.14.0");
    for (size_t renamed = 0; renamed <= next.files.size(); ++renamed) {
        makeCar(car, key, "0.13.0");
        put(car.root + "/opt/aa/evo-trial", "old", 0755);
        cutAfter(car, key, next, renamed);
        CHECK(check(car, "0.13.0") == evo::UpdateResult::kPending);  // no stick needed
        car.host.mountsText = "/dev/root / ext3 ro 0 0\n";           // the stick is not even there
        CHECK(install(car, "0.13.0") == evo::UpdateResult::kInstalled);
        CHECK(holdsRelease(car, next) && noNewFiles(car) && !exists(car.root + "/opt/aa/evo-trial"));
        CHECK(!exists(car.paths.pending));
        CHECK(!car.host.writable && car.host.restarts == 1);
    }
    // A rename refused halfway (here a folder where the service goes): the journal was written before the first
    // rename and stays; once the way is clear, the next update finishes from it, without the stick.
    makeCar(car, key, "0.13.0");
    putArchive(car, archiveName("0.14.0"), kit::archive(next, key));
    const std::string service = car.root + "/opt/aa/entry-aad";
    CHECK(unlink(service.c_str()) == 0 && mkdir(service.c_str(), 0755) == 0);
    put(service + "/in-the-way", "x");
    std::string report;
    CHECK(install(car, "0.13.0", &report) == evo::UpdateResult::kIncomplete);
    CHECK(report.find("cannot put /opt/aa/entry-aad in place") != std::string::npos);
    CHECK(exists(car.paths.pending) && car.host.restarts == 0 && !car.host.writable);
    CHECK(read(car.root + "/opt/aa/aa-supervisor") == next.files[0].content);  // renamed before the refusal
    CHECK(read(service + ".new") == next.files[1].content);
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kNewer);  // the stick's archive comes first
    removeTree(service);
    car.host.mountsText = "/dev/root / ext3 ro 0 0\n";
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kPending);
    CHECK(install(car, "0.13.0") == evo::UpdateResult::kInstalled);
    CHECK(holdsRelease(car, next) && noNewFiles(car) && !exists(car.paths.pending) && car.host.restarts == 1);
    // A .new lost: the journal cannot finish; the stick's archive does, and removes the journal.
    makeCar(car, key, "0.13.0");
    cutAfter(car, key, next, 3);
    unlink((car.root + "/" + next.files[5].path + ".new").c_str());
    const std::map<std::string, std::string> before = carFiles(car);
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kIncomplete);
    CHECK(install(car, "0.13.0") == evo::UpdateResult::kIncomplete);
    CHECK(carFiles(car) == before && car.host.restarts == 0);
    putArchive(car, archiveName("0.14.0"), kit::archive(next, key));
    CHECK(install(car, "0.13.0") == evo::UpdateResult::kInstalled);
    CHECK(holdsRelease(car, next) && noNewFiles(car) && !exists(car.paths.pending));
    // A journal signed by someone else is not followed.
    makeCar(car, key, "0.13.0");
    cutAfter(car, stranger, next, 2);
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kIncomplete);
    CHECK(install(car, "0.13.0") == evo::UpdateResult::kIncomplete && car.host.remounts == 0);
    // Every file already new, the journal still there: an install of the same archive only removes it.
    makeCar(car, key, "0.13.0");
    cutAfter(car, key, next, next.files.size());
    putArchive(car, archiveName("0.14.0"), kit::archive(next, key));
    CHECK(install(car, "0.14.0") == evo::UpdateResult::kUpToDate);
    CHECK(!exists(car.paths.pending) && car.host.remounts == 0);
    // An interrupted update, then a newer archive on the stick: the stick wins, the journal goes.
    makeCar(car, key, "0.13.0");
    cutAfter(car, key, next, 4);
    const kit::Release newer = kit::release("0.14.1");
    putArchive(car, archiveName("0.14.1"), kit::archive(newer, key));
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kNewer);
    CHECK(install(car, "0.13.0") == evo::UpdateResult::kInstalled);
    CHECK(holdsRelease(car, newer) && noNewFiles(car) && !exists(car.paths.pending));
    (void)old;
    printf("PASS journal\n");
}

// ---- what the car side decides from the car's answers ---------------------------------------------------------------
void carDecisions() {
    std::string why;
    // The car's loader (glibc 2.15 under QEMU, LD_TRACE_LOADED_OBJECTS), as it answered for real programs.
    const std::string good =
        "\tlibdbus-1.so.3 => /usr/lib/libdbus-1.so.3 (0x40025000)\n"
        "\tlibcrypto.so.1.0.0 => /lib/libcrypto.so.1.0.0 (0x4006c000)\n"
        "\tlibc.so.6 => /lib/libc.so.6 (0x401f0000)\n"
        "\t/lib/ld-linux-armhf.so.3 (0x40000000)\n";
    CHECK(evo::loaderFoundEverything(good, 0, why));
    CHECK(!evo::loaderFoundEverything(good + "\tlibxsvideo.so.4 => not found\n", 0, why));
    CHECK(why == "libxsvideo.so.4 => not found");
    CHECK(!evo::loaderFoundEverything("undefined symbol: SSL_foo\t(./entry-aad)\n" + good, 0, why));
    CHECK(!evo::loaderFoundEverything("./aa-supervisor: /lib/libc.so.6: version `GLIBC_2.17' not found\n", 1, why));
    CHECK(!evo::loaderFoundEverything("relocation error: symbol x, version y not defined in file z\n", 0, why));
    // What it says before listing anything, about a file it cannot map: a 64-bit or a cut program, exit 127.
    CHECK(!evo::loaderFoundEverything(
        "./entry-aad: error while loading shared libraries: ./entry-aad: wrong ELF class: ELFCLASS64\n", 127, why));
    CHECK(!evo::loaderFoundEverything("./x: error while loading shared libraries: libz.so.1\n", 0, why));
    CHECK(!evo::loaderFoundEverything("./entry-aad: Unable to run arch-specific checks\n", 127, why));
    CHECK(why == "the loader ended with 127: ./entry-aad: Unable to run arch-specific checks");
    CHECK(!evo::loaderFoundEverything("", 127, why) && why == "the loader ended with 127");
    CHECK(!evo::loaderFoundEverything(good, 1, why));

    // The system partition: the last mount on "/" is the one in use.
    CHECK(!evo::systemPartitionWritable("rootfs / rootfs rw 0 0\n/dev/root / ext3 ro,relatime 0 0\n"));
    CHECK(evo::systemPartitionWritable("/dev/root / ext3 ro 0 0\n/dev/root / ext3 rw,relatime 0 0\n"));
    CHECK(evo::systemPartitionWritable("/dev/root / ext3 rw 0 0\ntmpfs /tmp tmpfs ro 0 0\n"));
    CHECK(!evo::systemPartitionWritable("/dev/root / ext3 rwx 0 0\n") && !evo::systemPartitionWritable(""));
    CHECK(!evo::systemPartitionWritable("/dev/sda1 /media/sda1 tfat rw 0 0\n"));

    // One lock at a time, even within one process; free again once its holder closes it.
    const std::string lock = g_base + ".lock";
    const int first = evo::lockOwnFile(lock, 0);
    CHECK(first >= 0);
    CHECK(evo::lockOwnFile(lock, 0) < 0);
    CHECK(evo::lockOwnFile(lock, 300) < 0);  // tried a few times, still held
    close(first);
    const int again = evo::lockOwnFile(lock, 0);
    CHECK(again >= 0);
    close(again);
    unlink(lock.c_str());
    printf("PASS car decisions\n");
}

// ---- the install takes exactly what the check offered ---------------------------------------------------------------
void exactArchive(EVP_PKEY* key) {
    Car car;
    makeCar(car, key, "0.13.0");
    const kit::Release offered = kit::release("0.14.0"), later = kit::release("0.15.0");
    putArchive(car, archiveName("0.14.0"), kit::archive(offered, key));
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kNewer);
    CHECK(exists(car.paths.checked));
    putArchive(car, archiveName("0.15.0"), kit::archive(later, key));  // a newer one appears before the second press
    CHECK(install(car, "0.13.0") == evo::UpdateResult::kInstalled && holdsRelease(car, offered));
    CHECK(!exists(car.paths.checked));
    // The offered archive gone before the second press: nothing installed, even though another one is valid.
    makeCar(car, key, "0.13.0");
    putArchive(car, archiveName("0.14.0"), kit::archive(offered, key));
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kNewer);
    unlink((car.stick + "/" + archiveName("0.14.0")).c_str());
    putArchive(car, archiveName("0.15.0"), kit::archive(later, key));
    const std::map<std::string, std::string> before = carFiles(car);
    std::string report;
    CHECK(install(car, "0.13.0", &report) == evo::UpdateResult::kFailed);
    CHECK(report.find("no longer holds the archive the check found") != std::string::npos);
    CHECK(carFiles(car) == before && car.host.remounts == 0 && car.host.restarts == 0);
    // Without a check first (by hand, over SSH): the newest valid archive.
    CHECK(install(car, "0.13.0") == evo::UpdateResult::kInstalled && holdsRelease(car, later));
    // A check that offers nothing forgets an earlier offer.
    makeCar(car, key, "0.13.0");
    putArchive(car, archiveName("0.14.0"), kit::archive(offered, key));
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kNewer);
    unlink((car.stick + "/" + archiveName("0.14.0")).c_str());
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kNoPackage && !exists(car.paths.checked));
    printf("PASS exact archive\n");
}

// ---- a program the car could not run: refused before its loader is even asked ---------------------------------------
void notForThisCar(EVP_PKEY* key) {
    Car car;
    makeCar(car, key, "0.13.0");
    kit::Release wrong = kit::release("0.14.0");
    kit::Elf softFloat;
    softFloat.interpreter = "/lib/ld-linux.so.3";
    wrong.files[1].content = kit::elf(softFloat, "service");
    putArchive(car, archiveName("0.14.0"), kit::archive(wrong, key));
    evo::Updater updater(car.host, car.paths, car.keys, "0.13.0");
    CHECK(updater.check() == evo::UpdateResult::kIncompatible);
    CHECK(updater.report().find("/opt/aa/entry-aad is not for this car: asks for the loader /lib/ld-linux.so.3") !=
          std::string::npos);
    CHECK(car.host.loaded.empty());
    kit::Release script = kit::release("0.14.0");
    script.files[0].content = "#!/bin/sh\n";  // executable, but not a program
    putArchive(car, archiveName("0.14.0"), kit::archive(script, key));
    const std::map<std::string, std::string> before = carFiles(car);
    CHECK(install(car, "0.13.0") == evo::UpdateResult::kIncompatible && carFiles(car) == before);
    printf("PASS not for this car\n");
}

// ---- a journal left behind, the car already up to date: the menu offers to clear it ---------------------------------
void leftJournal(EVP_PKEY* key) {
    Car car;
    makeCar(car, key, "0.13.0");
    // An interrupted update to 0.14.0, then the car reinstalled at 0.13.0 by SSH: the journal cannot finish.
    const kit::Release installed = kit::release("0.13.0"), next = kit::release("0.14.0");
    put(car.paths.pending, hex(kit::sign(key, kit::listText(next))) + "\n" + kit::listText(next));
    putArchive(car, archiveName("0.13.0"), kit::archive(installed, key));
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kRepair);  // not "up to date": a second press is offered
    CHECK(install(car, "0.13.0") == evo::UpdateResult::kUpToDate);
    CHECK(!exists(car.paths.pending) && car.host.remounts == 0 && car.host.restarts == 0);
    CHECK(check(car, "0.13.0") == evo::UpdateResult::kUpToDate);
    printf("PASS left journal\n");
}

}  // namespace

int main() {
    char base[] = "/tmp/entry-aad-update-test-XXXXXX";
    CHECK(mkdtemp(base));
    g_base = base;
    EVP_PKEY* key = kit::makeKey(2048);
    EVP_PKEY* stranger = kit::makeKey(1024);
    checks(key, stranger);
    installs(key);
    refusals(key);
    journal(key, stranger);
    carDecisions();
    exactArchive(key);
    notForThisCar(key);
    leftJournal(key);
    removeTree(g_base);
    EVP_PKEY_free(key);
    EVP_PKEY_free(stranger);
    printf("PASS update install\n");
    return 0;
}
