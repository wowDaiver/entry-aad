// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto video: libxsvideo decoder, dma-buf feeding and surface visibility (see evo_video.hpp).
#include "evo_video.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "evo_log.hpp"

namespace evo {

static bool xsvCommand(const char* name, int result) {
    ELOG("Video", "%s: %s (result=%d)", name, result < 0 ? "FAILED" : "accepted", result);
    return result >= 0;
}

EvoVideoConfig::EvoVideoConfig()
    : width(1280),
      height(720),
      displayWidth(1280),
      displayHeight(480),
      fps(0),
      surfaceId(kAaSurfaceId),
      layerId(kAaLayerId),
      bufCount(8),
      bufSize(1u << 20),
      holdCloseFrames(false) {}

EvoVideo::EvoVideo()
    : h_(nullptr),
      running_(false),
      visible_(false),
      opacity_(1.f),
      ready_(false),
      started_(false),
      configSent_(false),
      evStarted_(false),
      seenFirstFrame_(false),
      framesIn_(0),
      framesDropped_(0),
      idrSeen_(0),
      framesAtStart_(0) {
    pthread_mutex_init(&mu_, nullptr);
    // The wait for a free buffer must not stretch or end early when the car sets its clock (GPS, RTC at boot).
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&cv_, &attr);
    pthread_condattr_destroy(&attr);
}

EvoVideo::~EvoVideo() {
    stop();
    pthread_cond_destroy(&cv_);
    pthread_mutex_destroy(&mu_);
}

void EvoVideoConfig::setDisplay(uint32_t canvasWidth, uint32_t canvasHeight, uint32_t panelWidth,
                                uint32_t panelHeight) {
    width = canvasWidth;
    height = canvasHeight;
    displayWidth = panelWidth;
    displayHeight = panelHeight;
    if (canvasWidth > 1280 || canvasHeight > 720) {
        bufSize = 2u << 20;
    }
}

bool EvoVideo::init(const EvoVideoConfig& cfg) {
    if (started_) {
        return false;
    }
    if (!cfg.width || !cfg.height || cfg.width > 1920 || cfg.height > 1080 || !cfg.bufCount || cfg.bufCount > 16 ||
        cfg.bufSize < 65536 || cfg.bufSize > (4u << 20) || cfg.displayWidth > cfg.width ||
        cfg.displayHeight > cfg.height ||
        !ilm_.validateTarget(cfg.surfaceId, cfg.layerId, cfg.displayWidth, cfg.displayHeight)) {
        ELOG("Video", "REFUSED: target not checked (inventory, free id, visible full-screen layer)");
        return false;
    }
    pool_.destroy();  // Also release a previous init() whose start() failed.
    cfg_ = cfg;
    ELOG("Video", "init %ux%u surface 0x%x layer 0x%x (%u dma-bufs of %u B)", cfg_.width, cfg_.height, cfg_.surfaceId,
         cfg_.layerId, cfg_.bufCount, cfg_.bufSize);
    return pool_.init(cfg_.bufCount, cfg_.bufSize);
}

bool EvoVideo::start() {
    if (started_) {
        return true;
    }
    if (!ilm_.validateTarget(cfg_.surfaceId, cfg_.layerId, cfg_.displayWidth, cfg_.displayHeight)) {
        ELOG("Video", "layer unreadable or surface already taken: giving up");
        return false;
    }
    h_ = xsv_open(XSV_TYPE_DIGITAL_CAM, cfg_.surfaceId, "entry-aad");
    if (!h_) {
        ELOG("Video", "xsv_open(type 1, surface 0x%x) failed", cfg_.surfaceId);
        return false;
    }
    if (!xsvCommand("xsv_configure_stream",
                    xsv_configure_stream(h_, cfg_.width, cfg_.height, static_cast<float>(cfg_.fps)))) {
        xsv_close(h_);
        h_ = nullptr;
        return false;
    }
    if (!xsvCommand("xsv_set_output_dimension", xsv_set_output_dimension(h_, cfg_.width, cfg_.height)) ||
        !xsvCommand("xsv_enable_frame_events", xsv_enable_frame_events(h_)) ||
        !xsvCommand("xsv_update", xsv_update(h_))) {
        xsv_close(h_);
        h_ = nullptr;
        return false;
    }
    if (!xsvCommand("xsv_start", xsv_start(h_))) {
        xsv_close(h_);
        h_ = nullptr;
        return false;
    }
    running_ = true;
    started_ = true;
    configSent_ = false;
    framesAtStart_ = framesIn_;
    stall_.restart();  // a new decoder; no frame is posted before start() returns
    pthread_mutex_lock(&mu_);
    gap_.restart(cfg_.holdCloseFrames);  // its counts are taken by the main loop
    pthread_mutex_unlock(&mu_);
    if (pthread_create(&evThread_, nullptr, eventStub, this) == 0) {
        evStarted_ = true;
    } else {
        ELOG("Video", "pthread_create(event) failed");
        stop();
        return false;
    }
    // Attach our surface hidden, cropped to the panel's area.
    if (!ilm_.attachSurface(cfg_.surfaceId, cfg_.layerId, cfg_.displayWidth, cfg_.displayHeight)) {
        stop();
        return false;
    }
    if (!ilm_.setSurfaceVisibility(cfg_.surfaceId, false)) {
        stop();
        return false;
    }
    // A reconnect creates a new surface: do not reuse the previous surface's opacity cache.
    opacity_ = 1.f;
    ready_ = true;
    ELOG("Video", "started (handle %p)", (void*)h_);
    return true;
}

void EvoVideo::stop() {
    if (!started_) {
        pool_.destroy();
        return;
    }
    running_ = false;
    pthread_mutex_lock(&mu_);
    pthread_cond_broadcast(&cv_);
    pthread_mutex_unlock(&mu_);
    if (evStarted_) {
        pthread_join(evThread_, nullptr);
        evStarted_ = false;
    }
    // Hide even if our cached flag is false (the library may have changed visibility).
    ilm_.setSurfaceVisibility(cfg_.surfaceId, false);
    ilm_.detachSurface(cfg_.surfaceId, cfg_.layerId);
    if (h_) {
        xsv_stop(h_);
        xsv_close(h_);
        h_ = nullptr;
    }
    pool_.destroy();
    started_ = false;
    visible_ = false;
    ready_ = false;
    ELOG("Video", "stopped (received=%llu dropped=%llu idr=%llu)", (unsigned long long)framesIn_,
         (unsigned long long)framesDropped_, (unsigned long long)idrSeen_);
}

void EvoVideo::setVisible(bool visible) {
    if (!started_) {
        return;
    }
    if (visible && !ready_) {
        return;
    }
    if (visible == visible_) {
        return;
    }
    // Only our surface: never hide the other surfaces of an HMI/CarPlay layer.
    // The selected layer must already be visible and in the screen's render order.
    if (ilm_.setSurfaceVisibility(cfg_.surfaceId, visible)) {
        visible_ = visible;
    }
}
bool EvoVideo::setOpacity(float opacity) {
    if (!started_) {
        return false;
    }
    if (opacity == opacity_) {
        return true;
    }
    if (!ilm_.setSurfaceOpacity(cfg_.surfaceId, opacity)) {
        return false;
    }
    opacity_ = opacity;
    return true;
}
bool EvoVideo::firstFrameSeen() {
    pthread_mutex_lock(&mu_);
    bool seen = seenFirstFrame_;
    pthread_mutex_unlock(&mu_);
    return seen;
}

void EvoVideo::postCodecConfig(const uint8_t* data, size_t len) {
    if (!data || !len) {
        return;
    }
    pthread_mutex_lock(&mu_);
    codecCfg_.assign(data, data + len);
    configSent_ = false;  // prepended to the next IDR
    pthread_mutex_unlock(&mu_);
    ELOG("Video", "codec config (SPS/PPS) %zu B cached", len);
}

// Looks for a NAL unit of type 5 (IDR) in an Annex B stream.
bool EvoVideo::isIdr(const uint8_t* d, size_t n) const {
    for (size_t i = 0; i + 4 < n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0) {
            size_t j = (d[i + 2] == 1) ? i + 3 : (d[i + 2] == 0 && d[i + 3] == 1) ? i + 4 : 0;
            if (j && (d[j] & 0x1f) == 5) {
                return true;
            }
        }
    }
    return false;
}

FrameFate EvoVideo::postVideoFrame(const uint8_t* data, size_t len, uint64_t pts) {
    (void)pts;
    if (!running_ || !started_) {
        return FrameFate::kNoStream;
    }

    pthread_mutex_lock(&mu_);
    // A frame right behind the previous one waits for the rest of kFrameGapMs, without the lock (FrameGap).
    const uint32_t holdMs = gap_.arrive(monoMs());
    if (holdMs) {
        pthread_mutex_unlock(&mu_);
        usleep(holdMs * 1000);
        pthread_mutex_lock(&mu_);
        if (!running_) {
            pthread_mutex_unlock(&mu_);
            return FrameFate::kNoStream;
        }
    }
    bool idr = isIdr(data, len);
    if (idr) {
        ++idrSeen_;
    }
    // SPS/PPS go in front of the first IDR after each (re)start.
    bool prependCfg = idr && !configSent_ && !codecCfg_.empty();
    size_t need = len + (prependCfg ? codecCfg_.size() : 0);

    int bi = pool_.acquireFree();
    // Bounded wait for a free buffer (the event thread releases one on each DMABUF_COMPLETE); none once the decoder
    // has stalled, so that the session thread no longer loses 20 ms on every frame.
    const bool wait = bi < 0 && !stall_.stuck(monoMs());
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    while (bi < 0 && wait && running_) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);  // the clock cv_ was created with
        ts = now;
        ts.tv_nsec += 20 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        if (pthread_cond_timedwait(&cv_, &mu_, &ts) == ETIMEDOUT) {
            break;
        }
        bi = pool_.acquireFree();
    }
    if (bi < 0) {
        ++framesDropped_;
        const bool stalled = stall_.starved(monoMs(), visible_);
        pthread_mutex_unlock(&mu_);
        if (stalled) {
            ELOG("Video", "no buffer back from the decoder for %u ms, all %u out: decoder stalled",
                 DecoderStall::kLimitMs, cfg_.bufCount);
            return FrameFate::kStalled;
        }
        return FrameFate::kLost;
    }
    stall_.bufferBack();

    DmaBufPool::Buf& b = pool_.at(bi);
    if (need > b.size) {
        ++framesDropped_;
        pthread_mutex_unlock(&mu_);
        ELOG("Video", "frame %zu B > buffer %u B", need, b.size);
        return FrameFate::kLost;
    }
    size_t off = 0;
    if (prependCfg) {
        memcpy(b.map, &codecCfg_[0], codecCfg_.size());
        off = codecCfg_.size();
        configSent_ = true;
    }
    memcpy(b.map + off, data, len);
    b.inFlight = true;
    int fd = b.fd;
    ++framesIn_;
    pthread_mutex_unlock(&mu_);

    int rc = xsv_decode_video_frame(h_, fd, static_cast<uint32_t>(need));
    if (rc < 0) {
        pthread_mutex_lock(&mu_);
        b.inFlight = false;
        if (prependCfg) {
            configSent_ = false;  // the decoder never got SPS/PPS: put them in front of the next IDR again
        }
        pthread_cond_signal(&cv_);
        pthread_mutex_unlock(&mu_);
        return FrameFate::kLost;
    }
    return FrameFate::kDecoding;
}

FrameGapStats EvoVideo::takeFrameGaps() {
    pthread_mutex_lock(&mu_);
    const FrameGapStats taken = gap_.take();
    pthread_mutex_unlock(&mu_);
    return taken;
}

void* EvoVideo::eventStub(void* self) {
    (static_cast<EvoVideo*>(self))->eventLoop();
    return nullptr;
}

void EvoVideo::eventLoop() {
    int evt[5];
    while (running_) {
        memset(evt, 0, sizeof(evt));
        xsv_receive_event(h_, evt, 200);  // 20 bytes: [0] type, [1] data
        switch (evt[0]) {
            case 0:
                break;  // timeout, nothing received
            case XSV_EVENT_PLAYING:
                ELOG("Video", "player PLAYING; waiting for H.264 frames");
                break;
            case XSV_EVENT_FRAME: {
                // A compositor callback is useful evidence, not proof the driver can see the panel.
                // event[1..4] are uninitialized in the HU callback and must never be interpreted/logged.
                pthread_mutex_lock(&mu_);
                bool first = !seenFirstFrame_ && framesIn_ > framesAtStart_;
                if (first) {
                    seenFirstFrame_ = true;
                }
                pthread_mutex_unlock(&mu_);
                if (first) {
                    ELOG("Video",
                         "first frame callback from the compositor after H.264; look at the screen to confirm");
                }
                break;
            }
            case XSV_EVENT_DMABUF_COMPLETE: {
                pthread_mutex_lock(&mu_);
                int bi = pool_.find(evt[1]);
                if (bi >= 0) {
                    pool_.at(bi).inFlight = false;
                    stall_.bufferBack();
                    pthread_cond_signal(&cv_);
                }
                pthread_mutex_unlock(&mu_);
                break;
            }
            case XSV_EVENT_STOPPED:
                ELOG("Video", "stream stopped");
                break;
            case XSV_EVENT_ERROR:
                ELOG("Video", "decoding error: %d", evt[1]);
                break;
            case XSV_EVENT_WATCHDOG_ERROR:
                ELOG("Video", "video watchdog");
                break;
            case XSV_EVENT_ABORTED:
                ELOG("Video", "stream aborted");
                break;
            default:
                ELOG("Video", "unexpected event: %d (%d)", evt[0], evt[1]);
                break;
        }
    }
}

}  // namespace evo
