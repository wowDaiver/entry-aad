// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Actual EvoVideo lifecycle with fake hardware boundaries. No DRM, compositor or vehicle connection.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "evo_video.hpp"

static unsigned height = 0, opacityCommands = 0, decodeFailures = 0;
static uint32_t decoded = 0;  // length of the last buffer handed to the decoder
static bool visible = false;
static float opacity = 1.f;
static bool decoderHoldsAll = false;  // a stalled decoder: no buffer ever comes back
static uint8_t bufferMemory[256];
namespace evo {
// One buffer, always free: the fake decoder never holds it, unless it stalled.
DmaBufPool::DmaBufPool() : drmFd_(-1), dev_(0) {}
DmaBufPool::~DmaBufPool() {}
bool DmaBufPool::init(unsigned, uint32_t) {
    Buf b = {nullptr, 7, bufferMemory, sizeof(bufferMemory), false};
    bufs_.assign(1, b);
    return true;
}
void DmaBufPool::destroy() {
    bufs_.clear();
}
int DmaBufPool::acquireFree() {
    return bufs_.empty() || decoderHoldsAll ? -1 : 0;
}
int DmaBufPool::find(int fd) {
    return !bufs_.empty() && fd == 7 ? 0 : -1;
}
EvoIlm::EvoIlm() {}
bool EvoIlm::validateTarget(uint32_t, uint32_t, uint32_t, uint32_t, bool) {
    return true;
}
bool EvoIlm::attachSurface(uint32_t, uint32_t, uint32_t, uint32_t h) {
    height = h;
    opacity = 1.f;
    visible = false;
    return true;
}
bool EvoIlm::detachSurface(uint32_t, uint32_t) {
    return true;
}
bool EvoIlm::setSurfaceVisibility(uint32_t, bool v) {
    visible = v;
    return true;
}
bool EvoIlm::setSurfaceOpacity(uint32_t, float v) {
    ++opacityCommands;
    opacity = v;
    return true;
}
}  // namespace evo
extern "C" {
xsv_handle xsv_open(int, uint32_t, const char*) {
    return reinterpret_cast<void*>(1);
}
void xsv_close(xsv_handle) {}
int xsv_configure_stream(xsv_handle, uint32_t, uint32_t, float) {
    return 0;
}
int xsv_set_output_dimension(xsv_handle, uint32_t, uint32_t) {
    return 0;
}
int xsv_enable_frame_events(xsv_handle) {
    return 0;
}
int xsv_update(xsv_handle) {
    return 0;
}
int xsv_start(xsv_handle) {
    return 0;
}
int xsv_stop(xsv_handle) {
    return 0;
}
int xsv_decode_video_frame(xsv_handle, int fd, uint32_t len) {
    assert(fd == 7);
    decoded = len;
    if (decodeFailures) {
        --decodeFailures;
        return -1;
    }
    return 0;
}
int xsv_receive_event(xsv_handle, int*, int) {
    usleep(1000);
    return 0;
}
}

static const uint8_t kSpsPps[] = {0, 0, 0, 1, 0x67, 1, 2, 3, 0, 0, 0, 1, 0x68, 4};
static const uint8_t kIdr[] = {0, 0, 0, 1, 0x65, 9, 9, 9, 9};
static const uint8_t kSlice[] = {0, 0, 0, 1, 0x41, 5, 5};

// The decoder received SPS/PPS followed by the IDR frame.
static bool configThenIdr() {
    return decoded == sizeof(kSpsPps) + sizeof(kIdr) && !memcmp(bufferMemory, kSpsPps, sizeof(kSpsPps)) &&
           !memcmp(bufferMemory + sizeof(kSpsPps), kIdr, sizeof(kIdr));
}

static long long nowMs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<long long>(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

// When a decoder that keeps every buffer counts as stalled, on a clock of our own.
static void decoderStallRule() {
    const uint32_t limit = evo::DecoderStall::kLimitMs;
    evo::DecoderStall stall;
    // On screen: reported once, when no buffer was free for the whole limit.
    const uint32_t t0 = 0xfffffc00u;  // across the clock's wrap-around
    assert(!stall.starved(t0, true) && !stall.stuck(t0));
    assert(!stall.starved(t0 + limit - 1, true) && !stall.stuck(t0 + limit - 1));
    assert(stall.starved(t0 + limit, true) && stall.stuck(t0 + limit));
    assert(!stall.starved(t0 + limit + 500, true) && stall.stuck(t0 + limit + 500));  // once
    stall.bufferBack();
    assert(!stall.stuck(t0 + limit + 600));
    assert(!stall.starved(t0 + limit + 700, true) && !stall.starved(t0 + 3 * limit, true));  // still once
    // A buffer back before the limit starts the count again.
    stall.restart();
    assert(!stall.starved(1000, true) && !stall.starved(1000 + limit - 1, true));
    stall.bufferBack();
    assert(!stall.starved(1000 + limit, true) && !stall.stuck(1000 + limit + limit - 1));
    assert(stall.starved(1000 + limit + limit, true));
    // Hidden: never counted, and the count starts when the picture is shown again.
    stall.restart();
    assert(!stall.starved(0, false) && !stall.starved(2 * limit, false) && !stall.stuck(2 * limit));
    assert(!stall.starved(2 * limit + 1, true) && !stall.starved(3 * limit, true));
    assert(!stall.starved(3 * limit + 1, false) && !stall.starved(3 * limit + 2, true));
    assert(!stall.starved(4 * limit + 1, true) && stall.starved(4 * limit + 2, true));
}

// libxsvideo gives no buffer back: once the limit has passed on screen, the frame says so once, and the frames after
// it no longer wait 20 ms each for a buffer. A new start() feeds a decoder again.
static void stalledDecoder(evo::EvoVideo& video, const evo::EvoVideoConfig& cfg) {
    typedef evo::FrameFate Fate;
    assert(video.init(cfg) && video.start());
    video.setVisible(true);
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
    decoderHoldsAll = true;
    const long long since = nowMs();
    Fate fate = Fate::kLost;
    unsigned dropped = 0;
    while (fate == Fate::kLost && nowMs() - since < evo::DecoderStall::kLimitMs + 2000) {
        fate = video.postVideoFrame(kSlice, sizeof(kSlice), 0);
        ++dropped;
    }
    const long long lasted = nowMs() - since;
    assert(fate == Fate::kStalled && lasted >= evo::DecoderStall::kLimitMs && dropped > 1);
    const long long after = nowMs();
    for (int i = 0; i < 10; ++i) {
        assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kLost);  // reported once
    }
    assert(nowMs() - after < 50);  // 10 frames, no 20 ms wait each
    video.stop();
    decoderHoldsAll = false;
    assert(video.init(cfg) && video.start());
    video.setVisible(true);
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
    video.stop();
}

// Which frames FrameGap holds, and for how long, on a clock of our own.
static void frameGapRule() {
    const uint32_t gap = evo::kFrameGapMs;
    evo::FrameGap frames;
    frames.restart(true);
    const uint32_t t = 0xffffff80u;             // wraps around between t + 103 and t + 150
    assert(frames.arrive(t) == 0);              // the first frame of a stream
    assert(frames.arrive(t + 33) == 0);         // evenly spaced
    assert(frames.arrive(t + 33 + gap) == 0);   // exactly the gap
    assert(frames.arrive(t + 100) == 0);        // a pair: the first goes at once,
    assert(frames.arrive(t + 102) == gap - 2);  // the second waits for the rest of the gap
    assert(frames.arrive(t + 103) == 0);        // never the frame after a held one
    assert(frames.arrive(t + 103) == gap);      // the longest hold: no time at all since the previous frame
    assert(frames.arrive(t + 150) == 0);
    assert(frames.arrive(t + 150 + gap - 1) == 1);
    evo::FrameGapStats seen = frames.take();
    assert(seen.frames == 9 && seen.early == 4 && seen.held == 3 && seen.longestHoldMs == gap);
    seen = frames.take();  // counted again from zero
    assert(seen.frames == 0 && seen.early == 0 && seen.held == 0 && seen.longestHoldMs == 0);
    // Whatever the arrivals, no hold exceeds the gap and two holds never follow each other.
    uint32_t now = 5, seed = 12345;
    bool heldBefore = false;
    for (int i = 0; i < 2000; ++i) {
        seed = seed * 1103515245u + 12345u;
        now += (seed >> 16) % 60;  // 0 to 59 ms apart
        const uint32_t hold = frames.arrive(now);
        assert(hold <= gap && !(hold && heldBefore));
        heldBefore = hold != 0;
    }
    // Without holding (USB): early frames are counted, none held.
    frames.restart(false);
    assert(frames.arrive(10) == 0 && frames.arrive(11) == 0 && frames.arrive(12) == 0);
    seen = frames.take();
    assert(seen.frames == 3 && seen.early == 2 && seen.held == 0 && seen.longestHoldMs == 0);
    // A new stream has no previous frame.
    frames.restart(true);
    assert(frames.arrive(12) == 0);
}

// The hold runs in postVideoFrame over Wi-Fi only; its counts reach the caller. In real time: frames posted back to
// back reach postVideoFrame within kFrameGapMs of each other only when this machine is not busy elsewhere, so each case
// gets a few tries (FrameGap's own timings are checked above with a clock of its own). What holds whatever the timing
// is checked on every try.
static void framesSpread(evo::EvoVideo& video, evo::EvoVideoConfig cfg) {
    typedef evo::FrameFate Fate;
    const int kTries = 5;
    bool spread = false;
    for (int i = 0; i < kTries && !spread; ++i) {
        cfg.holdCloseFrames = true;
        assert(video.init(cfg) && video.start());
        long long since = nowMs();
        assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
        assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kDecoding);  // held
        const long long pair = nowMs() - since;
        since = nowMs();
        // Queued behind the held frame: it arrives a gap after it, and would not be held anyway.
        assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kDecoding);
        const long long afterHeld = nowMs() - since;
        const evo::FrameGapStats seen = video.takeFrameGaps();
        video.stop();
        // Whatever the timing: three frames, never two holds in a row, only an early frame held, and its hold waited.
        assert(seen.frames == 3 && seen.held <= 1 && seen.held <= seen.early);
        assert(seen.held == 0 || pair + afterHeld + 1 >= static_cast<long long>(seen.longestHoldMs));
        spread = pair >= evo::kFrameGapMs - 5 && afterHeld < 15 && seen.early == 1 && seen.held == 1 &&
                 seen.longestHoldMs >= evo::kFrameGapMs - 5;
    }
    assert(spread);
    // Over the cable: at once, only counted.
    bool counted = false;
    for (int i = 0; i < kTries && !counted; ++i) {
        cfg.holdCloseFrames = false;
        assert(video.init(cfg) && video.start());
        const long long since = nowMs();
        assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding);
        assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kDecoding);
        const long long took = nowMs() - since;
        const evo::FrameGapStats seen = video.takeFrameGaps();
        video.stop();
        assert(seen.frames == 2 && seen.held == 0);  // never held over the cable, whatever the timing
        counted = took < 15 && seen.early == 1;
    }
    assert(counted);
}

int main() {
    typedef evo::FrameFate Fate;
    evo::EvoVideoConfig cfg;
    evo::EvoVideo video;
    assert(!video.isReady());
    assert(video.init(cfg) && video.start() && video.isReady() && height == 480);
    video.setVisible(true);
    assert(video.isVisible() && visible);
    assert(video.setOpacity(0.25f) && opacity == 0.25f);
    const unsigned n = opacityCommands;
    assert(video.setOpacity(0.25f) && opacityCommands == n);  // no repeated command on each main-loop tick
    assert(video.setOpacity(1.f) && opacity == 1.f);
    video.setVisible(false);  // camera/menu always hides the whole AA surface
    assert(!visible && !video.isVisible());
    video.setVisible(true);
    assert(visible);

    // SPS/PPS go in front of the first IDR; a frame the decoder refuses keeps them for the next IDR.
    video.postCodecConfig(kSpsPps, sizeof(kSpsPps));
    assert(video.postVideoFrame(kSlice, sizeof(kSlice), 0) == Fate::kDecoding && decoded == sizeof(kSlice));
    decodeFailures = 1;
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kLost && configThenIdr());
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding && configThenIdr());
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding && decoded == sizeof(kIdr));  // sent once

    video.stop();
    assert(!visible && !video.isVisible() && !video.isReady());
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kNoStream);  // stopped: nothing lost
    assert(video.init(cfg) && video.start() && video.isReady());
    assert(opacity == 1.f && video.setOpacity(0.25f) && opacity == 0.25f);  // fresh surface, fresh cache
    assert(video.postVideoFrame(kIdr, sizeof(kIdr), 0) == Fate::kDecoding && configThenIdr());  // SPS/PPS again
    video.stop();
    puts("PASS video presentation: visibility, opacity cache, camera hide, SPS/PPS after a refused IDR, reconnect");

    decoderStallRule();
    stalledDecoder(video, cfg);
    puts("PASS video presentation: a decoder that keeps every buffer 3 s on screen is reported once, then no waiting");
    frameGapRule();
    framesSpread(video, cfg);
    puts(
        "PASS video presentation: over Wi-Fi a frame within 25 ms of the previous one waits the rest, never two in a "
        "row; USB at once");
}
