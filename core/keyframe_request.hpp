// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// When the head unit may ask the phone for a new keyframe after the decoder lost a video frame.
#ifndef ENTRY_AAD_CORE_KEYFRAME_REQUEST_HPP
#define ENTRY_AAD_CORE_KEYFRAME_REQUEST_HPP

#include <stdint.h>

namespace aa {

// H.264 frames between two keyframes refer to the ones before them: once one is lost, the picture stays broken
// until the phone's next keyframe, which can be seconds away. Android Auto has no message to ask for one, but a
// phone that gets the video focus back starts its encoder again with a keyframe; the session takes the focus away
// and gives it back at once (aa_session.cpp).
// Not during the first seconds of the video, when the phone's own keyframes are still coming, and seldom: a decoder
// short of buffers would otherwise turn every frame into a keyframe, each one larger than the frames it replaces.
// Session thread only; the clock is the caller's, in milliseconds.
class KeyframeRequest {
public:
    static const uint64_t kSettleMs = 5000;    // after the video was set up
    static const uint64_t kSpacingMs = 10000;  // between two requests

    KeyframeRequest() : videoSinceMs_(0), lastMs_(0), video_(false), asked_(false) {}

    // The phone set the video channel up: a new stream, nothing asked yet.
    void videoStarted(uint64_t nowMs) {
        video_ = true;
        videoSinceMs_ = nowMs;
        asked_ = false;
    }
    // A new session.
    void reset() { video_ = asked_ = false; }
    // A frame was lost: true when a request may go now, and it then counts as made.
    bool allow(uint64_t nowMs) {
        if (!video_ || nowMs - videoSinceMs_ < kSettleMs || (asked_ && nowMs - lastMs_ < kSpacingMs)) {
            return false;
        }
        asked_ = true;
        lastMs_ = nowMs;
        return true;
    }

private:
    uint64_t videoSinceMs_, lastMs_;
    bool video_, asked_;
};

}  // namespace aa

#endif  // ENTRY_AAD_CORE_KEYFRAME_REQUEST_HPP
