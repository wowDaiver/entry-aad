// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Car input for the phone: bounded queues under one lock, coalesced where only the latest value matters.
#include "car_inbox.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>

#include "mutex_lock.hpp"

namespace aa {
namespace {

const size_t kMaxTouches = 256;
const size_t kMaxKeys = 64;
const size_t kMaxMicrophoneChunks = 64;  // about 1 s of 16 ms chunks: older audio is useless to the Assistant
const size_t kMaxMicrophoneChunk = 8192;
const unsigned kDropsLogged = 5;
const int kTouchMove = 2;  // TouchEventOut::action

void setNonBlocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

}  // namespace

CarInbox::CarInbox()
    : touchForwarding_(true),
      inputBound_(false),
      keyDrops_(0),
      rotaryDrops_(0),
      focus_(kFocusNone),
      keyframe_(false),
      night_(-1),
      nightStarted_(false),
      nightChanged_(false),
      microphoneOpen_(false) {
    pthread_mutex_init(&mutex_, nullptr);
    // Not optional, unlike touch: keys, the microphone and the video focus all reach the session through it.
    if (pipe(wake_) == 0) {
        setNonBlocking(wake_[1]);
        setNonBlocking(wake_[0]);
    } else {
        wake_[0] = wake_[1] = -1;
    }
}

CarInbox::~CarInbox() {
    for (int i = 0; i < 2; ++i) {
        if (wake_[i] >= 0) {
            close(wake_[i]);
        }
    }
    pthread_mutex_destroy(&mutex_);
}

void CarInbox::wake() {
    if (wake_[1] >= 0) {
        const char byte = 1;
        const ssize_t written = write(wake_[1], &byte, 1);
        (void)written;  // a full pipe already wakes the session thread
    }
}

void CarInbox::emptyWake() {
    char junk[64];
    while (read(wake_[0], junk, sizeof(junk)) > 0) {
    }
}

void CarInbox::postTouch(const TouchEventOut& touch) {
    if (!touchForwarding_) {
        return;
    }
    {
        MutexLock lock(mutex_);
        if (!inputBound_) {
            return;
        }
        // Moves in a row: only the newest positions matter.
        if (touch.action == kTouchMove && !touches_.empty() && touches_.back().action == kTouchMove) {
            touches_.back() = touch;
        } else if (touches_.size() < kMaxTouches) {
            touches_.push_back(touch);
        }
    }
    wake();
}

unsigned CarInbox::queueKey(const CarKey& key, unsigned& drops) {
    MutexLock lock(mutex_);
    if (!inputBound_ || keys_.size() >= kMaxKeys) {
        return ++drops;
    }
    // Rotary steps not sent yet add up.
    if (key.rotary && !keys_.empty() && keys_.back().rotary && keys_.back().keycode == key.keycode) {
        keys_.back().delta += key.delta;
    } else {
        keys_.push_back(key);
    }
    return 0;
}

void CarInbox::postKey(uint32_t keycode, bool down) {
    const CarKey key = {keycode, down, false, 0};
    const unsigned drops = queueKey(key, keyDrops_);
    if (!drops) {
        wake();
    } else if (drops <= kDropsLogged) {
        printf("[AASession] key %u %s dropped (input channel not bound)\n", keycode, down ? "down" : "up");
    }
}

void CarInbox::postRotary(uint32_t keycode, int delta) {
    const CarKey key = {keycode, false, true, delta};
    const unsigned drops = queueKey(key, rotaryDrops_);
    if (!drops) {
        wake();
    } else if (drops <= kDropsLogged) {
        printf("[AASession] rotary %d dropped (input channel not bound)\n", delta);
    }
}

void CarInbox::postFocus(bool projected) {
    if (wake_[1] < 0) {
        printf("[AASession] video focus %s not sent (no wake pipe)\n", projected ? "PROJECTED" : "NATIVE");
        return;
    }
    {
        MutexLock lock(mutex_);
        focus_ = projected ? kFocusProjected : kFocusNative;
    }
    wake();
}

void CarInbox::postKeyframeRequest() {
    bool first;
    {
        MutexLock lock(mutex_);
        first = !keyframe_;
        keyframe_ = true;
    }
    if (first) {  // a stalled decoder loses frame after frame: one wake is enough
        wake();
    }
}

void CarInbox::postNight(bool night) {
    bool due;
    {
        MutexLock lock(mutex_);
        const int state = night ? 1 : 0;
        if (state != night_ && nightStarted_) {
            nightChanged_ = true;
        }
        night_ = state;
        due = nightChanged_;
    }
    if (due) {
        wake();
    }
}

void CarInbox::postLocation(const CarLocation& location) {
    bool started;
    {
        MutexLock lock(mutex_);
        location_.latest = location;
        started = location_.started;
    }
    if (started) {
        wake();
    }
}

void CarInbox::postMicrophone(const uint8_t* pcm, size_t size) {
    if (!pcm || size == 0 || size > kMaxMicrophoneChunk) {
        return;
    }
    bool queued;
    {
        MutexLock lock(mutex_);
        queued = microphoneOpen_ && microphone_.size() < kMaxMicrophoneChunks;
        if (queued) {
            microphone_.push_back(std::vector<uint8_t>(pcm, pcm + size));
        }
    }
    if (queued) {
        wake();
    }
}

void CarInbox::take(CarBatch& batch) {
    batch.touches.clear();
    batch.keys.clear();
    batch.microphone.clear();
    MutexLock lock(mutex_);
    batch.touches.swap(touches_);
    batch.keys.swap(keys_);
    batch.focus = focus_;
    focus_ = kFocusNone;
    batch.keyframe = keyframe_;
    keyframe_ = false;
    batch.nightChanged = nightChanged_ && nightStarted_;
    nightChanged_ = false;
    batch.microphone.swap(microphone_);
}

std::vector<std::vector<uint8_t>> CarInbox::takeLocation(uint64_t nowMs) {
    MutexLock lock(mutex_);
    return location_.take(nowMs);
}

void CarInbox::deferFocus(FocusRequest focus) {
    MutexLock lock(mutex_);
    if (focus_ == kFocusNone) {
        focus_ = focus;
    }
}

int CarInbox::nightState() {
    MutexLock lock(mutex_);
    return night_;
}

void CarInbox::bindInput() {
    MutexLock lock(mutex_);
    inputBound_ = true;
    touches_.clear();
    keys_.clear();
}

void CarInbox::startNightMode() {
    MutexLock lock(mutex_);
    nightStarted_ = true;
    nightChanged_ = false;
}

void CarInbox::startLocation(uint32_t periodMs) {
    MutexLock lock(mutex_);
    location_.subscribe(periodMs);
}

void CarInbox::setMicrophoneOpen(bool open) {
    MutexLock lock(mutex_);
    microphoneOpen_ = open;
    if (!open) {
        microphone_.clear();
    }
}

void CarInbox::stopLocation() {
    MutexLock lock(mutex_);
    location_.reset();
}

void CarInbox::reset() {
    MutexLock lock(mutex_);
    inputBound_ = false;
    touches_.clear();
    keys_.clear();
    keyDrops_ = rotaryDrops_ = 0;
    focus_ = kFocusNone;
    keyframe_ = false;
    nightStarted_ = nightChanged_ = false;
    location_.reset();
    microphoneOpen_ = false;
    microphone_.clear();
}

}  // namespace aa
