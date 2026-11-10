// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The session's thread and wire: start and stop, frames in and out, the silence limit, the car input sent out.
#include "aa_session.hpp"

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "aa_protocol.hpp"
#include "crash_handler.hpp"
#include "mutex_lock.hpp"

namespace aa {
namespace {

// Silence from the phone after which it is gone: a wireless phone can vanish without closing the socket, and its last
// picture would stay on screen. It pings every second and reports its media status every second, even while iDrive
// has the screen. Measured on the clock (tests/test_session_silence): car input wakes the thread about 10 times a
// second. 5 s, so that a short Wi-Fi gap does not end the session.
const uint64_t kSilenceMs = 5000;
const unsigned kFramesLoggedFirst = 40;  // per channel, then one frame in kFramesLoggedEvery
const unsigned kFramesLoggedEvery = 300;
const unsigned kKeysLogged = 200;  // evo_knob logs every key anyway
const unsigned kMovesLogged = 40;
const unsigned kMicrophoneLogEvery = 250;
const int kTouchMove = 2;

uint64_t monotonicUs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000ULL + static_cast<uint64_t>(now.tv_nsec / 1000);
}

void printHead(const Message& message) {
    printf("            Data: ");
    for (size_t i = 0; i < message.size() && i < 16; ++i) {
        printf("%02X ", message[i]);
    }
    printf(message.size() > 16 ? "...\n" : "\n");
}

}  // namespace

AASession::AASession()
    : transport_(nullptr),
      running_(false),
      thread_(0),
      vehicleFile_(nullptr),
      softwareVersion_(nullptr),
      ioFailed_(false),
      videoReady_(false),
      mediaMessages_(0),
      microphoneChunks_(0) {
    pthread_mutex_init(&lifecycleLock_, nullptr);
    pthread_mutex_init(&socketLock_, nullptr);
    memset(mediaSessions_, 0, sizeof(mediaSessions_));
    audioMessages_[0] = audioMessages_[1] = 0;
    resetCounters();
}

AASession::~AASession() {
    stop();
    pthread_mutex_destroy(&socketLock_);
    pthread_mutex_destroy(&lifecycleLock_);
}

void AASession::setDisconnectCallback(SessionEndedCallback cb, void* user_data) {
    ended_.fn = cb;
    ended_.user = user_data;
}

void AASession::setVideoFrameCallback(VideoFrameCallback cb, void* user_data) {
    video_.fn = cb;
    video_.user = user_data;
}

void AASession::setCodecConfigCallback(CodecConfigCallback cb, void* user_data) {
    codec_.fn = cb;
    codec_.user = user_data;
}

void AASession::setVideoFocusCallback(VideoFocusCallback cb, void* user_data) {
    focus_.fn = cb;
    focus_.user = user_data;
}

void AASession::setMicRequestCallback(MicRequestCallback cb, void* user_data) {
    microphone_.fn = cb;
    microphone_.user = user_data;
}

void AASession::setNavStatusCallback(NavStatusCallback cb, void* user_data) {
    navigation_.fn = cb;
    navigation_.user = user_data;
}

void AASession::postTouchEvent(const TouchEventOut& ev) {
    inbox_.postTouch(ev);
}

void AASession::postKeyEvent(uint32_t keycode, bool down) {
    inbox_.postKey(keycode, down);
}

void AASession::postRelativeEvent(uint32_t keycode, int delta) {
    inbox_.postRotary(keycode, delta);
}

void AASession::postVideoFocus(bool projected) {
    inbox_.postFocus(projected);
}

void AASession::postNightMode(bool night) {
    inbox_.postNight(night);
}

void AASession::postCarLocation(const CarLocation& location) {
    inbox_.postLocation(location);
}

void AASession::postMicData(const uint8_t* pcm, size_t len) {
    inbox_.postMicrophone(pcm, len);
}

// ---- lifecycle ----------------------------------------------------------------------------------------------------

bool AASession::running() const {
    MutexLock lock(socketLock_);
    return running_;
}

void AASession::setRunning(bool on) {
    MutexLock lock(socketLock_);
    running_ = on;
}

bool AASession::clearRunning() {
    MutexLock lock(socketLock_);
    const bool was = running_;
    running_ = false;
    return was;
}

void AASession::interrupt() {
    MutexLock lock(socketLock_);
    if (transport_) {
        transport_->cancel();
    }
}

void AASession::closeTransport() {
    Transport* stream;
    {
        MutexLock lock(socketLock_);
        stream = transport_;
        transport_ = nullptr;
    }
    delete stream;
}

void AASession::resetCounters() {
    keysLogged_ = movesLogged_ = navigationLogged_ = pings_ = 0;
    playbackState_ = kNoField;
    memset(framesSeen_, 0, sizeof(framesSeen_));
}

bool AASession::start(int client_fd) {
    return start(socketTransport(client_fd));
}

bool AASession::start(Transport* transport) {
    MutexLock lifecycle(lifecycleLock_);
    // A session still there (the phone came back before its silence was noticed, or one that ended and was never
    // joined): wake it and join it. Clearing the flag first keeps its thread from running the ended callback; it runs
    // here, for a session that was still running only.
    const bool wasRunning = running();
    setRunning(false);
    interrupt();
    if (thread_) {
        pthread_join(thread_, nullptr);
        thread_ = 0;
    }
    closeTransport();
    if (wasRunning && ended_.fn) {
        ended_.fn(ended_.user);
    }

    {
        MutexLock lock(socketLock_);
        transport_ = transport;
    }
    if (!transport) {
        return false;
    }
    assembler_.clear();
    inbox_.reset();
    ioFailed_ = false;
    videoReady_ = false;
    microphoneChunks_ = 0;
    mediaMessages_ = 0;
    memset(mediaSessions_, 0, sizeof(mediaSessions_));
    audioMessages_[0] = audioMessages_[1] = 0;
    resetCounters();
    setRunning(true);
    if (pthread_create(&thread_, nullptr, &AASession::threadMain, this) != 0) {
        perror("[AASession] Failed to spawn session thread");
        setRunning(false);
        closeTransport();
        thread_ = 0;
        return false;
    }
    return true;
}

void AASession::stop() {
    MutexLock lifecycle(lifecycleLock_);
    setRunning(false);
    interrupt();
    // Joined even when the session ended by itself, or its thread's resources would leak.
    if (thread_) {
        pthread_join(thread_, nullptr);
        thread_ = 0;
    }
    closeTransport();
}

void* AASession::threadMain(void* self) {
    static_cast<AASession*>(self)->run();
    return nullptr;
}

void AASession::run() {
    printf("[AASession] Protocol thread active on %s.\n", transport_->name());
    printf("[AASession] Sending VersionRequest (v%u.%u)...\n", kVersionMajor, kVersionMinor);
    if (!sendPlain(channel::kControl, message::versionRequest())) {
        printf("[AASession] Error: Failed to send VersionRequest.\n");
        finish();
        return;
    }
    while (running() && !ioFailed_) {
        uint8_t channel = 0;
        Message message;
        const Received received = receive(channel, message);
        if (received == kLost) {
            break;
        }
        if (received == kMessage && !dispatch(channel, message)) {
            break;
        }
    }
    printf("[AASession] Session loop terminated.\n");
    g_trail.stage = kTrailIdle;
    finish();
}

void AASession::finish() {
    inbox_.stopLocation();
    // This thread alone closes the socket, whether the session ended by itself or stop() asked; the ended callback
    // runs only in the first case.
    const bool endedByItself = clearRunning();
    closeTransport();
    if (endedByItself && ended_.fn) {
        ended_.fn(ended_.user);
    }
}

// ---- the wire -----------------------------------------------------------------------------------------------------

bool AASession::receiveExactly(uint8_t* to, size_t size) {
    size_t got = 0;
    uint64_t lastDataMs = locationClockMs();
    const int wakeFd = inbox_.wakeFd();
    while (got < size && running()) {
        bool woke = false;
        const ssize_t n = transport_->read(to + got, size - got, wakeFd, woke);
        if (woke) {
            inbox_.emptyWake();
            sendCarInput();
        }
        if (n < 0 || ioFailed_) {
            return false;
        }
        if (n == 0) {
            const uint64_t silentMs = locationClockMs() - lastDataMs;
            if (silentMs >= kSilenceMs) {
                printf("[AASession] No data from phone for %u s: treating session as dead.\n",
                       static_cast<unsigned>(silentMs / 1000));
                return false;
            }
            continue;
        }
        lastDataMs = locationClockMs();
        got += static_cast<size_t>(n);
    }
    return got == size;
}

bool AASession::sendExactly(const uint8_t* data, size_t size) {
    size_t sent = 0;
    const uint64_t deadline = locationClockMs() + kSilenceMs;
    while (sent < size && running()) {
        if (locationClockMs() >= deadline) {
            ioFailed_ = true;
            return false;
        }
        const ssize_t n = transport_->write(data + sent, size - sent);
        if (n < 0) {
            ioFailed_ = true;
            return false;
        }
        if (n == 0) {
            continue;
        }
        sent += static_cast<size_t>(n);
    }
    if (sent != size) {
        return false;
    }
    if (!transport_->endWrite()) {
        ioFailed_ = true;
        return false;
    }
    return true;
}

bool AASession::sendFrame(uint8_t channel, uint8_t flags, const uint8_t* payload, size_t size) {
    std::vector<uint8_t> frame;
    return encodeFrame(channel, flags, payload, size, frame) && sendExactly(&frame[0], frame.size());
}

bool AASession::sendPlain(uint8_t channel, const Message& message) {
    return sendFrame(channel, frame::kPlain, &message[0], message.size());
}

bool AASession::sendSealed(uint8_t channel, const Message& message, uint8_t flags) {
    std::vector<uint8_t> records;
    return tls_.seal(&message[0], message.size(), records) && sendFrame(channel, flags, &records[0], records.size());
}

AASession::Received AASession::receive(uint8_t& channel, Message& message) {
    uint8_t raw[frame::kHeaderSize];
    if (!receiveExactly(raw, sizeof(raw))) {
        if (running()) {
            printf("[AASession] Phone transport closed or read error.\n");
        }
        return kLost;
    }
    FrameHeader header = decodeFrameHeader(raw);
    channel = header.channel;
    g_trail.stage = kTrailFrame;
    g_trail.channel = channel;
    const unsigned seen = framesSeen_[channel]++;
    const bool logged = seen < kFramesLoggedFirst || seen % kFramesLoggedEvery == 0;
    if (header.announcesSize()) {
        uint8_t total[frame::kTotalSizeSize];
        if (!receiveExactly(total, sizeof(total))) {
            printf("[AASession] Failed to read extended total_size header.\n");
            return kLost;
        }
        header.totalSize = decodeTotalSize(total);
        if (logged) {
            printf("[AASession] Frame RX (EXTENDED FIRST): Ch=%u, Flags=0x%02X, Len=%u, Total=%u\n", channel,
                   header.flags, header.length, header.totalSize);
        }
    } else if (logged) {
        printf("[AASession] Frame RX: Ch=%u, Flags=0x%02X, Len=%u\n", channel, header.flags, header.length);
    }
    if (header.length == 0) {
        return kNothing;
    }
    std::vector<uint8_t> payload(header.length);
    if (!receiveExactly(&payload[0], payload.size())) {
        printf("[AASession] Failed to read payload bytes (%u bytes).\n", header.length);
        return kLost;
    }
    g_trail.stage = kTrailDecrypt;
    std::vector<uint8_t> plain;
    if (!header.encrypted()) {
        plain.swap(payload);
    } else if (!tls_.open(&payload[0], payload.size(), plain)) {
        printf("[AASession] Failed to decrypt encrypted frame (%u bytes).\n", header.length);
        assembler_.drop(channel);
        return kNothing;
    }
    switch (assembler_.add(header, plain, message)) {
        case MessageAssembler::kComplete:
            break;
        case MessageAssembler::kPending:
            return kNothing;
        case MessageAssembler::kOverflow:
            printf("[AASession] Reassembly overflow on channel %u: dropping fragments.\n", channel);
            return kNothing;
        case MessageAssembler::kNoFirst:
            printf("[AASession] LAST fragment on channel %u with no FIRST: dropping\n", channel);
            return kNothing;
        case MessageAssembler::kShortOfSize:
            printf("[AASession] channel %u reassembled %u wire bytes, phone announced %u: dropping\n", channel,
                   assembler_.receivedBytes(), assembler_.announcedBytes());
            return kNothing;
    }
    if (logged) {
        printHead(message);
    }
    return kMessage;
}

// ---- car input ----------------------------------------------------------------------------------------------------

// Everything the car posted since the last turn, in a fixed order: location, microphone, night mode, video focus,
// keys, touches.
void AASession::sendCarInput() {
    CarBatch batch;
    inbox_.take(batch);
    if (!sendLocation()) {
        printf("[AASession] location -> phone: SEND FAILED\n");
    }
    for (size_t i = 0; i < batch.microphone.size(); ++i) {
        if (!sendSealed(channel::kMicrophone, message::micData(monotonicUs(), batch.microphone[i]))) {
            printf("[AASession] mic -> phone: SEND FAILED\n");
            break;
        }
        if ((microphoneChunks_++ % kMicrophoneLogEvery) == 0) {
            printf("[AASession] mic -> phone: %u chunks sent (%u bytes each)\n", microphoneChunks_,
                   static_cast<unsigned>(batch.microphone[i].size()));
        }
    }
    if (batch.nightChanged && !sendSensorEvent(sensor::kNightMode)) {
        printf("[AASession] NIGHT_MODE event: SEND FAILED\n");
    }
    if (batch.focus != kFocusNone && !videoReady_) {
        // Before the phone set the video up, a focus would mean nothing to it; the setup itself answers FOCUSED.
        inbox_.deferFocus(batch.focus);
    } else if (batch.focus != kFocusNone) {
        const bool projected = batch.focus == kFocusProjected;
        const bool sent = sendVideoFocus(projected, true);
        printf("[AASession] video focus -> phone: %s (unrequested) %s\n",
               projected ? "FOCUSED/projected" : "UNFOCUSED/native", sent ? "sent" : "SEND FAILED");
    }
    for (size_t i = 0; i < batch.keys.size(); ++i) {
        const CarKey& key = batch.keys[i];
        const Message event = key.rotary ? message::rotaryEvent(monotonicUs(), key.keycode, key.delta)
                                         : message::keyEvent(monotonicUs(), key.keycode, key.down);
        const bool sent = sendSealed(channel::kInput, event);
        if (!sent || keysLogged_++ < kKeysLogged) {
            if (key.rotary) {
                printf("[AASession] rotary -> phone: key=%u delta=%d (%s)\n", key.keycode, key.delta,
                       sent ? "sent" : "SEND FAILED");
            } else {
                printf("[AASession] key -> phone: key=%u %s (%s)\n", key.keycode, key.down ? "down" : "up",
                       sent ? "sent" : "SEND FAILED");
            }
        }
        if (!sent) {
            return;
        }
    }
    for (size_t i = 0; i < batch.touches.size(); ++i) {
        const TouchEventOut& touch = batch.touches[i];
        const bool sent = sendSealed(channel::kInput, message::touchEvent(monotonicUs(), touch));
        if (touch.action != kTouchMove || movesLogged_ < kMovesLogged) {
            if (touch.action == kTouchMove) {
                ++movesLogged_;
            }
            printf("[AASession] touch -> phone: action=%d idx=%d n=%d first=(%d,%d) (%s)\n", touch.action, touch.index,
                   touch.n, touch.n ? touch.p[0].x : -1, touch.n ? touch.p[0].y : -1, sent ? "sent" : "SEND FAILED");
        }
        if (!sent) {
            break;
        }
    }
}

bool AASession::sendLocation() {
    const std::vector<std::vector<uint8_t>> messages = inbox_.takeLocation(locationClockMs());
    for (size_t i = 0; i < messages.size(); ++i) {
        if (!sendSealed(channel::kSensors, messages[i])) {
            return false;
        }
        if (messageId(messages[i]) == msg::kSensorError) {
            printf("[AASession] car position %s\n", messages[i].back() == 1 ? "back" : "unavailable or stale");
        }
    }
    return true;
}

// Sensors with a source: the car's location, its day or night, and a driving status that never restricts.
bool AASession::sendSensorEvent(uint32_t type) {
    if (type == sensor::kLocation) {
        return sendLocation();
    }
    if (type == sensor::kNightMode) {
        const int state = inbox_.nightState();
        const bool night = state > 0;
        printf("[AASession] >>> Sensor NIGHT_MODE event (is_night=%u, %s)\n", night ? 1 : 0,
               state < 0 ? "car state unknown yet: day" : "car day/night");
        return sendSealed(channel::kSensors, message::nightModeEvent(night));
    }
    if (type == sensor::kDrivingStatus) {
        printf("[AASession] >>> Sensor DRIVING_STATUS event (status=%u)\n", sensor::kDrivingUnrestricted);
        return sendSealed(channel::kSensors, message::drivingStatusEvent(sensor::kDrivingUnrestricted));
    }
    return true;  // no source: the start response alone
}

bool AASession::sendVideoFocus(bool projected, bool unrequested) {
    const bool sent = sendSealed(channel::kVideo, message::videoFocusIndication(projected, unrequested));
    if (sent) {
        printf("[AASession] >>> Sent VideoFocusIndication (%s%s) on Channel %u!\n", projected ? "FOCUSED" : "UNFOCUSED",
               unrequested ? ", unrequested" : "", channel::kVideo);
    }
    return sent;
}

}  // namespace aa
