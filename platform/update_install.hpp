// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The update from a USB stick, behind the iDrive menu's "Update from USB stick" button: `aa-supervisor
// --update-check`, then `aa-supervisor --update-install` once the driver confirms.
//
// The stick holds the release archive as published, at its root (update_package.hpp). Nothing else is needed: no PC,
// no SSH. Only an archive signed by the project's key is taken.
//
// check  reads the sticks and says what an install would do. It writes nothing on the car but its report: the archive
//        found and its version against the installed one, every file checked against the signed list, each new
//        program checked against the car (ELF header, loader asked for) and given to the car's own loader (every
//        library and symbol it needs is there), and whether the car already holds exactly these files. It notes
//        which archive it found, so that the install takes that one and no other.
// install does all of that again, from the archive read once more into memory, then:
//   1. writes each file that differs next to the old one as <name>.new, flushed to the flash, then read back from it
//      and checked;
//   2. keeps the signed list on the data partition, in one file replaced at once: the journal;
//   3. renames each <name>.new over <name>. A rename replaces a file at once: each file is the old one or the new one,
//      never half of each, and a program in use keeps running from the old one;
//   4. removes the files of the two folders the new version does not have, then the journal; the unit restarts.
// The system partition is made writable for step 1. The kernel only takes it back to read-only once nothing uses a
// replaced file any more, and the iDrive keeps its app open: it stays writable until the restart that follows, a few
// seconds. A power cut before step 2 leaves the old version, plus .new files the next update removes. After step 2
// the next update finishes the renames from the journal, with or without the stick. A failure before the first rename
// puts nothing in place: "nothing changed".
//
// Only the two folders of an installation by this project are touched (each holds .installed-by-entryaad); the
// settings and logs of /var/local/data/AA stay. Each run writes what it found and did into a report, on the data
// partition and on the stick (EntryAA-update.txt), for the driver to send. tests/test_update_install.cpp.
#ifndef ENTRY_AAD_PLATFORM_UPDATE_INSTALL_HPP
#define ENTRY_AAD_PLATFORM_UPDATE_INSTALL_HPP

#include <string>
#include <vector>

#include "log_copy.hpp"
#include "update_package.hpp"

namespace evo {

// Also the exit codes of `aa-supervisor --update-check` and `--update-install`, read by the iDrive app.
enum class UpdateResult : int {
    kInstalled = 0,      // installed and checked; the unit restarts a few seconds later
    kNoStick = 3,        // no USB stick (the same code as LogCopyResult::kNoStick)
    kNewer = 10,         // check: a newer version is on the stick
    kOlder = 11,         // check: an older version is on the stick
    kRepair = 12,        // check: the installed version, but the car's files differ from it (or a journal is left)
    kPending = 13,       // check: no update on the stick, but an interrupted one the journal can finish
    kUpToDate = 14,      // the car already holds exactly the files of the archive
    kNoPackage = 15,     // a stick, without an update archive at its root
    kInvalid = 16,       // archives found, none intact and signed by the project
    kIncompatible = 17,  // a new program is not for this car, or the car's loader misses what it needs
    kBusy = 18,          // Android Auto or another update runs
    kNotInstalled = 19,  // the folders are not those of an installation by this project: nothing is touched
    kFailed = 20,        // refused or failed before the first rename: nothing changed
    kIncomplete = 21,    // part of the update is in place: run it again from the stick
};
const char* updateResultText(UpdateResult result);

// What the engine needs from the car, so that the tests can stand in for it.
class UpdateHost {
public:
    virtual ~UpdateHost() {}
    virtual std::string mounts() = 0;  // as /proc/mounts
    virtual bool systemWritable() = 0;
    // Remounts the system partition; true when it then is as asked.
    virtual bool setSystemWritable(bool writable) = 0;
    // The car's loader finds every library and symbol of the program or library at path (nothing of it runs).
    virtual bool loads(const std::string& path, std::string& why) = 0;
    virtual long long systemFreeBytes() = 0;
    virtual void restartUnit() = 0;   // after an install: the new app only loads with a fresh iDrive
    virtual std::string stamp() = 0;  // the date, for the report
};

struct UpdatePaths {
    std::string root;         // before every path of the system: empty on the car, a folder in the tests
    std::string pending;      // the journal: the signed list being swapped in
    std::string report;       // what the last run found and did
    std::string checked;      // the list of the archive the last check offered, by its SHA-256
    std::string staging;      // a folder in memory the car's loader may map programs from
    std::string mediaPrefix;  // where the car mounts USB sticks
};
const char kUpdateStickReport[] = "EntryAA-update.txt";
const char kInstalledMarker[] = ".installed-by-entryaad";

class Updater {
public:
    // keys: the DER public keys an archive may be signed by. installedVersion: the version of the running programs.
    Updater(UpdateHost& host, const UpdatePaths& paths, const std::vector<std::string>& keys,
            const std::string& installedVersion);
    UpdateResult check();
    UpdateResult install();
    // What the last check or install found and did, as written to the report.
    const std::string& report() const { return report_; }
    const std::string& foundVersion() const { return package_.list.version; }

private:
    struct Plan {
        std::vector<size_t> changed;     // files of the list the car does not hold as listed
        std::vector<std::string> extra;  // paths in the two folders that are not in the list
        long long bytes;
        Plan() : bytes(0) {}
    };
    enum Found { kFoundNoStick, kFoundNothing, kFoundInvalid, kFoundValid, kFoundOther };

    UpdateResult run(bool install);
    UpdateResult checkNow();
    UpdateResult installNow();
    // want: the SHA-256 of the list to take (what the check offered), or empty for the newest valid archive.
    Found findPackage(const std::string& want);
    bool ourInstallation();
    Plan plan(const UpdateList& list);
    int programsLoad();
    bool beginWritable();
    void endWritable();
    bool writeNewFiles(const Plan& plan);
    bool writePending();
    bool swap(const UpdateList& list, const std::vector<size_t>& files);
    void removeExtra(const std::vector<std::string>& extra);
    void removePending();
    int readPending(UpdatePackage& journal);
    // The journal's renames: true with every file either in place or as a checked .new to rename.
    bool pendingFinishable(const UpdatePackage& journal, std::vector<size_t>& toRename);
    UpdateResult finishPending();
    void writeReports();
    void note(const std::string& line);
    std::string onSystem(const std::string& path) const { return paths_.root + "/" + path; }

    UpdateHost& host_;
    UpdatePaths paths_;
    std::vector<std::string> keys_;
    std::string installedVersion_;
    std::string report_;
    UpdatePackage package_;  // the archive chosen on the stick
    std::string packageStick_;
    std::vector<StickMount> sticks_;
    bool madeWritable_;
};

// ---- what the car side decides, pure so that the tests read the car's real outputs ------------------------------
// The car's loader, run on a program with LD_TRACE_LOADED_OBJECTS (docs/INSTALLATION.md, step 2), found everything:
// it ended with 0 and named no missing library, symbol or version. why: the first line that says otherwise.
bool loaderFoundEverything(const std::string& output, int exitCode, std::string& why);
// The system partition is mounted read-write: the last mount on "/" in mounts (/proc/mounts) is the one in use.
bool systemPartitionWritable(const std::string& mounts);
// An exclusive lock on a file of ours, tried for waitMs at most (another process may hold it a moment). The descriptor
// holding it, or -1.
int lockOwnFile(const std::string& path, unsigned waitMs);

// The car: its paths, the project's keys (update_key.hpp), one update at a time, Android Auto's lock taken for an
// install. Returns the result as the exit code.
int updateFromUsb(bool install, const char* installedVersion);
UpdatePaths carUpdatePaths();

}  // namespace evo

#endif  // ENTRY_AAD_PLATFORM_UPDATE_INSTALL_HPP
