// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Real startup/stop processes with fake BMW boundaries: cable changes never switch the running AA service.
#include <limits.h>

#include <atomic>
#include <cmath>
#include <fstream>
#include <thread>

#include "fake_usb_backend.hpp"

// GCC 4.7 headers remove the global math macro after <thread>; the old GPS header expects this declaration.
using std::isfinite;
#include "supervisor_fakes.hpp"

static volatile sig_atomic_t childStop = 0;
static void childSignal(int) {
    childStop = 1;
}

static int childMain(int argc, char** argv) {
    bool usb = false;
    int inherited = -1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--usb-port")) {
            usb = true;
        }
        if (!strcmp(argv[i], "--usb-lock-fd") && i + 1 < argc) {
            inherited = atoi(argv[++i]);
        }
    }
    if (usb) {
        assert(inherited >= 3 && fcntl(inherited, F_GETFD) >= 0);
        assert(flock(inherited, LOCK_EX | LOCK_NB) == 0);
        assert(access("bluetooth-retour.txt", F_OK) != 0);  // wireless cleanup must finish BEFORE USB spawn
    } else {
        assert(access("bluetooth-retour.txt", F_OK) == 0);
    }
    const int separate = open("lock", O_RDWR);
    assert(separate >= 0 && flock(separate, LOCK_EX | LOCK_NB) != 0);  // no gap in the parent's lock
    close(separate);
    assert(access("child-active", F_OK) != 0);  // the previous child finished cleanup before this one started
    FILE* active = fopen("child-active", "w");
    assert(active);
    fclose(active);
    FILE* events = fopen("child-events", "a");
    assert(events);
    fprintf(events, "%s\n", usb ? "USB" : "wireless");
    fclose(events);
    signal(SIGTERM, access("ignore-term", F_OK) == 0 ? SIG_IGN : childSignal);
    while (!childStop) {
        usleep(10000);
    }
    assert(unlink("child-active") == 0);
    return 0;
}

static unsigned eventCount() {
    std::ifstream f("child-events");
    std::string line;
    unsigned count = 0;
    while (std::getline(f, line)) {
        ++count;
    }
    return count;
}
static void waitEvent(unsigned count) {
    const uint32_t begin = evo::monoMs();
    while (eventCount() < count && evo::monoMs() - begin < 6000) {
        usleep(10000);
    }
    assert(eventCount() == count);
}
static void resetFakeBmw() {
    stopRequested = 0;
    simulated = evo::SppSlotSnapshot();
    const uint8_t generic[] = {1, 0x11, 1, 1, 0x11, 1};
    simulated.uuidPayload = simulated.transportUuidPayload = std::vector<uint8_t>(generic, generic + 6);
    simulated.state = 1;
    configuration = 0;
    liveLink = false;
    failRead = failUuid = failConfig = false;
    videoGone = true;
    evo::apAvailable = true;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--run")) {
        return childMain(argc, argv);
    }
    char binary[PATH_MAX];
    assert(realpath(argv[0], binary));
    char original[PATH_MAX];
    assert(getcwd(original, sizeof(original)));
    char directory[] = "/tmp/entry-aa-auto-supervisor-XXXXXX";
    assert(mkdtemp(directory) && chdir(directory) == 0);
    stopPath = "STOP";
    const int lock = open("lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    assert(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0);
    for (unsigned mode = 0; mode < 3; ++mode) {
        FakeUsbTree tree;
        const std::string port = tree.root + "/3-1", detached = tree.root + "/detached";
        aa::UsbSelection selection = tree.selection();
        selection.automatic = true;
        if (mode == 0) {
            assert(rename(port.c_str(), detached.c_str()) == 0);  // startup without a phone
        }
        if (mode == 2) {
            tree.identity(true);  // a previously enumerated accessory is physically present
        }
        resetFakeBmw();
        std::thread changes([&]() {
            waitEvent(1);
            if (mode == 0) {
                assert(rename(detached.c_str(), port.c_str()) == 0);
            } else {
                assert(rename(port.c_str(), detached.c_str()) == 0);
            }
            usleep(1100000);
            assert(eventCount() == 1 && access("child-active", F_OK) == 0);  // startup choice remains fixed
            FILE* stop = fopen(stopPath, "w");
            assert(stop);
            fclose(stop);
        });
        assert(selectAndRunService(binary, lock, 1, selection, tree.root.c_str(), 500) == 0);
        changes.join();
        assert(eventCount() == 1 && access("child-active", F_OK) != 0 && access(journal, F_OK) != 0);
        std::ifstream events("child-events");
        std::string all((std::istreambuf_iterator<char>(events)), {});
        assert(all == (mode == 0 ? "wireless\n" : "USB\n"));
        events.close();
        assert(unlink("child-events") == 0);
        if (mode != 0) {
            assert(rename(detached.c_str(), port.c_str()) == 0);
        }
    }
    {
        FakeUsbTree tree;
        aa::UsbSelection selection = tree.selection();
        selection.automatic = true;
        tree.field("devnum", "unreadable");
        resetFakeBmw();
        assert(selectAndRunService(binary, lock, 1, selection, tree.root.c_str(), 500) == 1);
        assert(eventCount() == 0);  // do not silently choose wireless when the probe failed
    }
    {
        FakeUsbTree tree;
        aa::UsbSelection selection = tree.selection();
        selection.automatic = true;
        resetFakeBmw();
        FILE* ignore = fopen("ignore-term", "w");
        assert(ignore);
        fclose(ignore);
        std::thread changes([&]() {
            waitEvent(1);
            usleep(100000);
            FILE* stop = fopen(stopPath, "w");
            assert(stop);
            fclose(stop);
        });
        assert(selectAndRunService(binary, lock, 1, selection, tree.root.c_str(), 200) == 1);
        changes.join();
        assert(eventCount() == 1);  // a forced stop is reported as failure, never restarted
    }
    unlink("child-active");
    unlink("child-events");
    unlink("ignore-term");
    unlink(stopPath);
    close(lock);
    unlink("lock");
    assert(chdir(original) == 0 && rmdir(directory) == 0);
    puts(
        "PASS actual supervisor: startup USB/wireless, fixed mode after hotplug, inherited lock, recovery, Stop, probe "
        "failure");
}
