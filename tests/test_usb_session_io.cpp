// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Session-level USB cancellation, dead writes and unplug must release resources and notify the app exactly once.
#include <atomic>
#include <ctime>

#include "aa_session.hpp"
#include "fake_usb_backend.hpp"

static uint64_t nowMs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<uint64_t>(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}
static void ended(void* counter) {
    ++*static_cast<std::atomic<unsigned>*>(counter);
}

class SessionUsb : public FakeUsbBackend {
public:
    enum Mode { kQuiet, kUnplug, kBlockedWrite };
    SessionUsb(FakeUsbTree& tree, FakeUsbStats& stats, Mode mode) : FakeUsbBackend(tree, stats), mode_(mode) {}
    int bulk(uint8_t endpoint, uint8_t*, int size, int& transferred, unsigned timeout) override {
        transferred = 0;
        if (endpoint == 0x81) {
            ++stats.bulkReads;
            if (mode_ == kUnplug) {
                return -4;
            }
            usleep(timeout * 1000);
            return -7;
        }
        assert(endpoint == 2);
        ++stats.bulkWrites;
        if (mode_ == kBlockedWrite) {
            usleep(timeout * 1000);
            return -7;
        }
        transferred = size;
        return 0;
    }

private:
    Mode mode_;
};

int main() {
    for (int mode = 0; mode < 3; ++mode) {
        FakeUsbTree tree;
        tree.identity(true);
        FakeUsbStats stats;
        std::atomic<unsigned> callbacks(0);
        aa::AASession session;
        session.setDisconnectCallback(ended, &callbacks);
        aa::Transport* stream =
            aa::connectUsb(tree.selection(true), nullptr,
                           new SessionUsb(tree, stats, static_cast<SessionUsb::Mode>(mode)), tree.root.c_str());
        assert(stream && session.start(stream));
        const uint64_t started = nowMs();
        if (mode == SessionUsb::kQuiet) {
            usleep(100000);
            const uint64_t stopping = nowMs();
            session.stop();
            assert(nowMs() - stopping < 250 && callbacks == 0);
        } else {
            while (session.isActive() && nowMs() - started < 6500) {
                usleep(10000);
            }
            assert(!session.isActive());
            session.stop();
            assert(callbacks == 1);
            if (mode == SessionUsb::kBlockedWrite) {
                assert(nowMs() - started >= 4900 && nowMs() - started < 6000);
            }
        }
        assert(stats.claims == 1 && stats.releases == 1 && stats.opens == stats.closes);
    }
    // Repeated stop while the session thread is blocked in a bulk OUT call.
    for (unsigned i = 0; i < 10; ++i) {
        FakeUsbTree tree;
        tree.identity(true);
        FakeUsbStats stats;
        std::atomic<unsigned> callbacks(0);
        aa::AASession session;
        session.setDisconnectCallback(ended, &callbacks);
        assert(session.start(aa::connectUsb(
            tree.selection(true), nullptr, new SessionUsb(tree, stats, SessionUsb::kBlockedWrite), tree.root.c_str())));
        usleep(30000);
        const uint64_t stopping = nowMs();
        session.stop();
        assert(nowMs() - stopping < 250 && callbacks == 0 && stats.releases == 1 && stats.opens == stats.closes);
    }
    puts("PASS USB session: write deadline, unplug callback, bounded concurrent stop, repeated cleanup");
}
