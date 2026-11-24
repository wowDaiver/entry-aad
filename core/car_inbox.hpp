// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// What the car posts for the phone, from any thread, kept until the session thread sends it.
#ifndef ENTRY_AAD_CORE_CAR_INBOX_HPP
#define ENTRY_AAD_CORE_CAR_INBOX_HPP

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "aa_input.hpp"
#include "car_location.hpp"

namespace aa {

// A key press or release, or rotary steps.
struct CarKey {
    uint32_t keycode;
    bool down;
    bool rotary;
    int delta;
};

enum FocusRequest { kFocusNone = 0, kFocusProjected = 1, kFocusNative = 2 };

// One turn's worth, taken at once by the session thread.
struct CarBatch {
    std::vector<TouchEventOut> touches;
    std::vector<CarKey> keys;
    FocusRequest focus;
    bool keyframe;      // the decoder lost a video frame since the last turn
    bool nightChanged;  // the phone started night mode and the car changed since the last event
    std::vector<std::vector<uint8_t>> microphone;
};

// One lock guards everything; a pipe wakes the session thread, which waits on it next to the phone's socket. Car
// input waits for the phone to bind the input channel and is dropped before; microphone audio flows while the phone
// keeps the microphone open; the location and night mode go once the phone started those sensors.
class CarInbox {
public:
    CarInbox();
    ~CarInbox();

    // Set before the session starts. Touches are dropped when off; keys still go.
    void setTouchForwarding(bool on) { touchForwarding_ = on; }

    // Any thread.
    void postTouch(const TouchEventOut& touch);
    void postKey(uint32_t keycode, bool down);
    void postRotary(uint32_t keycode, int delta);
    void postFocus(bool projected);
    void postKeyframeRequest();  // a video frame was lost: the session decides whether to ask the phone
    void postNight(bool night);  // kept across sessions
    void postLocation(const CarLocation& location);
    void postMicrophone(const uint8_t* pcm, size_t size);

    // Session thread.
    int wakeFd() const { return wake_[0]; }
    void emptyWake();
    void take(CarBatch& batch);
    std::vector<std::vector<uint8_t>> takeLocation(uint64_t nowMs);
    // The video is not set up yet: the focus waits for the next turn, unless a newer one came meanwhile.
    void deferFocus(FocusRequest focus);
    int nightState();  // -1 not known yet, 0 day, 1 night
    void bindInput();  // the phone bound the input channel: car input is accepted from now on
    void startNightMode();
    void startLocation(uint32_t periodMs);
    void setMicrophoneOpen(bool open);
    void stopLocation();
    // A new phone: everything of the previous session is forgotten but the car's day or night and last position.
    void reset();

private:
    void wake();
    unsigned queueKey(const CarKey& key, unsigned& drops);  // 0 when queued, else the drops of that kind so far

    pthread_mutex_t mutex_;
    int wake_[2];  // read end, write end; -1 when the pipe could not be made
    bool touchForwarding_;
    // Guarded by mutex_.
    bool inputBound_;
    std::vector<TouchEventOut> touches_;
    std::vector<CarKey> keys_;
    unsigned keyDrops_, rotaryDrops_;
    FocusRequest focus_;
    bool keyframe_;
    int night_;
    bool nightStarted_, nightChanged_;
    LocationFeed location_;
    bool microphoneOpen_;
    std::vector<std::vector<uint8_t>> microphone_;
};

}  // namespace aa

#endif  // ENTRY_AAD_CORE_CAR_INBOX_HPP
