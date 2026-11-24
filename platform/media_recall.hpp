// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Two short MEDIA clicks and a one-shot, session-bound request for the iDrive app to recall Android Auto.
#ifndef ENTRY_AAD_PLATFORM_MEDIA_RECALL_HPP
#define ENTRY_AAD_PLATFORM_MEDIA_RECALL_HPP

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <string>

namespace evo {

inline bool mediaRecallAllowed(bool connected, bool onScreen, bool stopping, bool call, bool reverse) {
    return connected && !onScreen && !stopping && !call && !reverse;
}

class MediaDoublePress {
public:
    static const uint32_t kShortMs = 500;
    static const uint32_t kPairMs = 500;
    // Another key or the knob was used: the gesture is cancelled, but MEDIA's known state stays. Forgetting it, as an
    // unknown state does, lost the next press on the older controller, which only reports edges: after MENU, the
    // first MEDIA press was taken for the state and a double press counted one click (F20, #55).
    static const unsigned kOtherInput = 8;

    MediaDoublePress() : seen_(false), down_(false), short_(false), first_(false), downAt_(0), firstAt_(0) {}

    // A guard or session changed: keep the observed physical state, but never reuse a click or a held press.
    void cancel() {
        short_ = false;
        first_ = false;
    }

    // Raw button state: 0 released, 1 pressed, 4 finger resting. Holds and unknown states cancel the gesture.
    // A first observation already pressed is not a new click. Duplicate status copies never make extra clicks.
    bool feed(unsigned state, uint32_t now, bool allowed) {
        if (state == kOtherInput) {
            cancel();
            return false;
        }
        if (state != 0 && state != 1 && state != 4) {
            cancel();
            seen_ = false;
            down_ = false;
            return false;
        }
        const bool down = state == 1;
        if (!seen_ || !allowed) {
            cancel();
            seen_ = true;
            down_ = down;
            return false;
        }
        if (down == down_) {
            return false;
        }
        down_ = down;
        if (down) {
            short_ = true;
            downAt_ = now;
            return false;
        }
        if (!short_ || static_cast<uint32_t>(now - downAt_) > kShortMs) {
            cancel();
            return false;
        }
        short_ = false;
        if (first_ && static_cast<uint32_t>(now - firstAt_) <= kPairMs) {
            first_ = false;
            return true;
        }
        first_ = true;
        firstAt_ = now;
        return false;
    }

private:
    bool seen_, down_, short_, first_;
    uint32_t downAt_, firstAt_;
};

// The service is the only writer; rename publishes a complete request, without changing the service's focus.
inline bool writeMediaRecall(const char* path, int pid, uint32_t now) {
    const std::string temporary = std::string(path) + ".tmp";
    FILE* file = fopen(temporary.c_str(), "w");
    if (!file) {
        return false;
    }
    const bool written = fprintf(file, "media-recall %d %u\n", pid, static_cast<unsigned>(now)) > 0;
    const bool closed = fclose(file) == 0;
    if (written && closed && rename(temporary.c_str(), path) == 0) {
        return true;
    }
    unlink(temporary.c_str());
    return false;
}

// Detach before reading: a newer request published meanwhile stays for the next poll. Every request is consumed,
// including one from an old process, a malformed file, or a request more than two monotonic seconds old.
inline bool takeMediaRecall(const char* path, int expectedPid, uint32_t now) {
    const std::string reading = std::string(path) + ".read";
    if (rename(path, reading.c_str()) != 0) {
        return false;
    }
    FILE* file = fopen(reading.c_str(), "r");
    int pid = 0;
    unsigned at = 0;
    char extra;
    const int fields = file ? fscanf(file, "media-recall %d %u %c", &pid, &at, &extra) : 0;
    if (file) {
        fclose(file);
    }
    unlink(reading.c_str());
    return fields == 2 && pid > 1 && pid == expectedPid && static_cast<uint32_t>(now - at) <= 2000;
}

}  // namespace evo
#endif
