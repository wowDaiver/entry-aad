// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto video: H.264 decoded by libxsvideo onto an IVI surface, shown and hidden through the LayerManager.

// The pieces:
//   libxsvideo type 1   H.264 decoder (IVA-HD/DCE) rendering NV12 onto an IVI surface; it connects to Wayland
//                       itself (see xsv_api.h).
//   OMAP dma-bufs       carry the Annex B H.264 stream to the decoder (omap_dmabuf.hpp).
//   LayerManager (ILM)  attaches and hides our surface in a layer that is already visible (evo_ilm.hpp).
//
// The phone renders one of its three canvases, chosen for the panel (core/display_plan.hpp); the surface is cropped
// to the top-left part the panel shows, which holds the phone's UI. On the reference car: a 1280x720 canvas, the
// panel is 1280x480 (omapdrm fb0 1280,480) and shows its top 480 lines.
#ifndef ENTRY_AAD_PLATFORM_EVO_VIDEO_HPP
#define ENTRY_AAD_PLATFORM_EVO_VIDEO_HPP

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <vector>

#include "evo_ilm.hpp"
#include "omap_dmabuf.hpp"
#include "xsv_api.h"

namespace evo {

// IVI ids of our surfaces (observed working on the car): Android Auto on the HMI's full-screen layer, the id checked
// free before each start. The volume pill takes the next id, above Android Auto.
const uint32_t kAaSurfaceId = 0x7eaa0001, kPillSurfaceId = 0x7eaa0002, kAaLayerId = 0x80000000;

struct EvoVideoConfig {
    uint32_t width, height;                // phone stream, its canvas: 1280x720
    uint32_t displayWidth, displayHeight;  // shown part of it, the panel: 1280x480
    uint32_t fps;                          // 0 = not imposed
    uint32_t surfaceId;                    // kAaSurfaceId
    uint32_t layerId;                      // kAaLayerId, already visible
    unsigned bufCount;                     // 8 dma-bufs
    uint32_t bufSize;                      // 1 MiB each
    bool holdCloseFrames;                  // the phone on Wi-Fi: frames that come in pairs are spread (FrameGap)
    EvoVideoConfig();                      // the reference car's values; frames never held (USB)
    // The stream and its shown part for a panel. A 1920x1080 stream gets larger buffers for its keyframes.
    void setDisplay(uint32_t canvasWidth, uint32_t canvasHeight, uint32_t panelWidth, uint32_t panelHeight);
};

// Frames that come too close together. Over Wi-Fi, frames can arrive in pairs a few milliseconds apart: the compositor
// draws only the latest buffer at each refresh, so the first of a pair never reaches the screen and the picture
// stutters although nothing is lost on the network.
// A frame that comes less than kFrameGapMs after the previous one is held for the rest of that time before it goes to
// the decoder. The frame after a held one is never held: holds never add up, no frame waits more than kFrameGapMs,
// and evenly spaced frames go through at once. Without holding (USB) early frames are only counted, for the log.
// Under EvoVideo's lock; monotonic milliseconds from the caller, compared with wrap-around.
const uint32_t kFrameGapMs = 25;  // under the 33 ms between two frames at 30 frames/s

// What FrameGap saw since its counts were last taken.
struct FrameGapStats {
    unsigned frames;         // that arrived
    unsigned early;          // less than kFrameGapMs after the previous one
    unsigned held;           // of those, held back
    uint32_t longestHoldMs;  // 0: none held
    FrameGapStats() : frames(0), early(0), held(0), longestHoldMs(0) {}
};

class FrameGap {
public:
    FrameGap() : lastMs_(0), hold_(false), seen_(false), lastHeld_(false) {}
    // A new stream. hold: early frames are held back (Wi-Fi), else only counted.
    void restart(bool hold) {
        hold_ = hold;
        seen_ = lastHeld_ = false;
        stats_ = FrameGapStats();
    }
    // A frame arrived at nowMs: how long to hold it before it goes to the decoder, 0 for at once.
    uint32_t arrive(uint32_t nowMs) {
        uint32_t holdMs = 0;
        const uint32_t sinceMs = nowMs - lastMs_;
        ++stats_.frames;
        if (seen_ && sinceMs < kFrameGapMs) {
            ++stats_.early;
            if (hold_ && !lastHeld_) {
                holdMs = kFrameGapMs - sinceMs;
                ++stats_.held;
                stats_.longestHoldMs = holdMs > stats_.longestHoldMs ? holdMs : stats_.longestHoldMs;
            }
        }
        // The previous frame went to the decoder when it arrived, unless held: then this one is not.
        seen_ = true;
        lastHeld_ = holdMs != 0;
        lastMs_ = nowMs;
        return holdMs;
    }
    // The counts since the previous call; they start over.
    FrameGapStats take() {
        const FrameGapStats taken = stats_;
        stats_ = FrameGapStats();
        return taken;
    }

private:
    uint32_t lastMs_;  // when the previous frame arrived
    bool hold_, seen_, lastHeld_;
    FrameGapStats stats_;
};

// What became of one frame handed to postVideoFrame.
enum class FrameFate {
    kDecoding,  // in the decoder
    kLost,      // dropped (no free buffer in time, larger than a buffer) or refused by the decoder: the frames after it
                // refer to a picture the decoder never had
    kNoStream,  // the decoder is not running
    kStalled,   // dropped, and the decoder gives no buffer back any more (DecoderStall): only a new one shows the
                // phone again. Once per start().
};

// Whether libxsvideo has stopped giving the dma-bufs back. Each buffer returns with DMABUF_COMPLETE; after a decoding
// error, its watchdog or an abort, none may come back. Every buffer then stays out, each later frame waits for one in
// vain and is dropped, and the picture stays frozen while the phone goes on streaming.
// Counted only while our surface is shown: whether libxsvideo still takes frames for a hidden surface was never seen
// (park assist hides it while the phone keeps streaming), and a hidden picture freezes nothing.
// Under EvoVideo's lock; monotonic milliseconds from the caller, compared with wrap-around.
class DecoderStall {
public:
    static const uint32_t kLimitMs = 3000;  // no buffer free for that long, on screen: stalled

    DecoderStall() : sinceMs_(0), starving_(false), reported_(false) {}
    // A new decoder.
    void restart() { starving_ = reported_ = false; }
    // A buffer came back, or a frame found one free.
    void bufferBack() { starving_ = false; }
    // A frame found no free buffer. True once per decoder: when it has been so for kLimitMs on screen.
    bool starved(uint32_t nowMs, bool shown) {
        if (!shown) {
            starving_ = false;  // starts again once shown
            return false;
        }
        if (!starving_) {
            starving_ = true;
            sinceMs_ = nowMs;
        }
        if (reported_ || !stuck(nowMs)) {
            return false;
        }
        reported_ = true;
        return true;
    }
    // Stalled: a frame is dropped at once instead of waiting for a buffer that will not come.
    bool stuck(uint32_t nowMs) const { return starving_ && nowMs - sinceMs_ >= kLimitMs; }

private:
    uint32_t sinceMs_;  // the first frame that found no buffer, while starving_
    bool starving_, reported_;
};

class EvoVideo {
public:
    EvoVideo();
    ~EvoVideo();

    bool init(const EvoVideoConfig& cfg);
    bool start();  // opens and configures libxsvideo, starts the event thread, attaches the hidden surface
    void stop();

    void setVisible(bool visible);
    bool setOpacity(float opacity);  // our surface only; cached (no CLI call when unchanged)
    bool isVisible() const { return visible_; }
    bool isReady() const { return ready_; }  // start() completed: the surface is attached and can be shown
    bool firstFrameSeen();                   // compositor frame callback after H.264; not a visual confirmation

    // SPS/PPS (the Android Auto codec config): kept and prepended to the first IDR frame after each (re)start.
    void postCodecConfig(const uint8_t* data, size_t len);
    // One H.264 access unit, copied into a free dma-buf and handed to the decoder. On Wi-Fi a frame that comes right
    // after the previous one waits up to kFrameGapMs first, on the calling thread (FrameGap).
    FrameFate postVideoFrame(const uint8_t* data, size_t len, uint64_t pts);
    // What FrameGap saw since the previous call (the periodic log line); any thread.
    FrameGapStats takeFrameGaps();

private:
    static void* eventStub(void* self);
    void eventLoop();
    bool isIdr(const uint8_t* data, size_t len) const;  // holds a NAL unit of type 5?

    EvoVideoConfig cfg_;
    EvoIlm ilm_;
    DmaBufPool pool_;
    xsv_handle h_;
    std::atomic<bool> running_;
    std::atomic<bool> visible_;
    float opacity_;
    bool ready_;
    bool started_;
    bool configSent_;
    pthread_t evThread_;
    bool evStarted_;
    pthread_mutex_t mu_;
    pthread_cond_t cv_;              // signalled when a dma-buf is released; waits use CLOCK_MONOTONIC
    std::vector<uint8_t> codecCfg_;  // cached SPS/PPS
    DecoderStall stall_;
    FrameGap gap_;
    bool seenFirstFrame_;
    uint64_t framesIn_, framesDropped_, idrSeen_, framesAtStart_;
};

}  // namespace evo

#endif  // ENTRY_AAD_PLATFORM_EVO_VIDEO_HPP
