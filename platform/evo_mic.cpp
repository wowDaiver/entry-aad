// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Captures the car microphone through the HU's libasound, on a thread woken by the phone's mic requests.
#include "evo_mic.hpp"

#include <dlfcn.h>
#include <errno.h>
#include <time.h>

#include <cstdlib>

#include "evo_log.hpp"

namespace evo {

namespace {
const int kStreamCapture = 1;                 // SND_PCM_STREAM_CAPTURE
const int kOpenNonBlocking = 1;               // SND_PCM_NONBLOCK: a read never waits for sound that may not come
const int kFormatS16LE = 2;                   // SND_PCM_FORMAT_S16_LE
const int kAccessRwInterleaved = 3;           // SND_PCM_ACCESS_RW_INTERLEAVED
const unsigned kRate = 16000, kFrames = 320;  // 20 ms per read
const char* const kPcm = "mic1";              // the car's microphone (observed working on the car)
// The longest the thread waits for sound in one go: how soon a close from the phone or stop() is seen.
const int kWaitSliceMs = 100;
// A capture that gives no frame for this long has failed: the phone's Assistant hears nothing anyway.
const uint32_t kSilenceMs = 2000;
// After a failed capture, before opening again: a capture that fails at once cannot spin the thread.
const unsigned kReopenPauseMs = 1000;
// Failed captures in a row that never gave a frame: the microphone is left alone until the phone asks again.
const unsigned kSilentFailuresBeforeRest = 3;

// The absolute CLOCK_MONOTONIC time `ms` from now, for pthread_cond_timedwait on cv_.
struct timespec monotonicDeadline(unsigned ms) {
    struct timespec at;
    clock_gettime(CLOCK_MONOTONIC, &at);
    at.tv_sec += ms / 1000;
    at.tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
    if (at.tv_nsec >= 1000000000L) {
        at.tv_sec += 1;
        at.tv_nsec -= 1000000000L;
    }
    return at;
}
}  // namespace

EvoMic::EvoMic()
    : cb_(nullptr),
      user_(nullptr),
      lib_(nullptr),
      started_(false),
      quit_(false),
      want_(false),
      asks_(0),
      open_(nullptr),
      setParams_(nullptr),
      readi_(nullptr),
      wait_(nullptr),
      recover_(nullptr),
      close_(nullptr),
      strerror_(nullptr) {
    pthread_mutex_init(&mu_, nullptr);
    // The pause before reopening is timed on the monotonic clock: the unit's wall clock can jump by hours.
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&cv_, &attr);
    pthread_condattr_destroy(&attr);
}

EvoMic::~EvoMic() {
    stop();
    if (lib_) {
        dlclose(lib_);
    }
    pthread_cond_destroy(&cv_);
    pthread_mutex_destroy(&mu_);
}

bool EvoMic::load() {
    lib_ = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
        ELOG("Mic", "libasound.so.2 not found: no microphone");
        return false;
    }
    *reinterpret_cast<void**>(&open_) = dlsym(lib_, "snd_pcm_open");
    *reinterpret_cast<void**>(&setParams_) = dlsym(lib_, "snd_pcm_set_params");
    *reinterpret_cast<void**>(&readi_) = dlsym(lib_, "snd_pcm_readi");
    *reinterpret_cast<void**>(&wait_) = dlsym(lib_, "snd_pcm_wait");
    *reinterpret_cast<void**>(&recover_) = dlsym(lib_, "snd_pcm_recover");
    *reinterpret_cast<void**>(&close_) = dlsym(lib_, "snd_pcm_close");
    *reinterpret_cast<void**>(&strerror_) = dlsym(lib_, "snd_strerror");
    if (!open_ || !setParams_ || !readi_ || !wait_ || !recover_ || !close_ || !strerror_) {
        ELOG("Mic", "libasound incomplete: no microphone");
        dlclose(lib_);
        lib_ = nullptr;
        return false;
    }
    return true;
}

bool EvoMic::start(DataCallback cb, void* user) {
    if (started_) {
        return true;
    }
    if (!lib_ && !load()) {
        return false;
    }
    cb_ = cb;
    user_ = user;
    quit_ = false;
    want_ = false;
    if (pthread_create(&thread_, nullptr, threadStub, this) != 0) {
        ELOG("Mic", "cannot start the thread");
        return false;
    }
    started_ = true;
    ELOG("Mic", "ready (%s, 16 kHz mono)", kPcm);
    return true;
}

void EvoMic::request(bool open) {
    pthread_mutex_lock(&mu_);
    want_ = open;
    if (open) {
        ++asks_;
    }
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
}

void EvoMic::stop() {
    if (!started_) {
        return;
    }
    pthread_mutex_lock(&mu_);
    quit_ = true;
    want_ = false;
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
    pthread_join(thread_, nullptr);
    started_ = false;
}

void* EvoMic::threadStub(void* arg) {
    static_cast<EvoMic*>(arg)->loop();
    return nullptr;
}

bool EvoMic::wanted() {
    pthread_mutex_lock(&mu_);
    const bool keep = want_ && !quit_;
    pthread_mutex_unlock(&mu_);
    return keep;
}

// Sleeps `ms`, woken early by a close from the phone or stop(). True if the phone still wants the mic.
bool EvoMic::pauseWhileWanted(unsigned ms) {
    const struct timespec until = monotonicDeadline(ms);
    pthread_mutex_lock(&mu_);
    int waited = 0;
    while (want_ && !quit_ && waited != ETIMEDOUT) {
        waited = pthread_cond_timedwait(&cv_, &mu_, &until);
    }
    const bool keep = want_ && !quit_;
    pthread_mutex_unlock(&mu_);
    return keep;
}

// Waits for the phone's next open request, a close or stop(): only a new request opens the microphone again.
void EvoMic::restUntilAskedAgain() {
    pthread_mutex_lock(&mu_);
    const unsigned seen = asks_;
    while (!quit_ && want_ && asks_ == seen) {
        pthread_cond_wait(&cv_, &mu_);
    }
    pthread_mutex_unlock(&mu_);
}

void* EvoMic::openCapture() {
    void* pcm = nullptr;
    int err = open_(&pcm, kPcm, kStreamCapture, kOpenNonBlocking);
    if (err >= 0) {
        err = setParams_(pcm, kFormatS16LE, kAccessRwInterleaved, 1, kRate, 1, 100000);
    }
    if (err < 0) {
        ELOG("Mic", "cannot open %s: %s", kPcm, strerror_(err));
        if (pcm) {
            close_(pcm);
        }
        return nullptr;
    }
    return pcm;
}

// Reads until the phone closes the mic or stop() (true), or until the capture fails (false): an error the stream
// cannot recover from, or no frame for kSilenceMs.
bool EvoMic::captureUntilClosed(void* pcm, unsigned long& frames, unsigned& errors) {
    int16_t buf[kFrames];
    uint32_t lastFrameMs = monoMs();
    while (wanted()) {
        // Read first: the first read starts the capture, and a wait on a stream not started yet would only time out.
        long n = readi_(pcm, buf, kFrames);
        if (n > 0) {
            frames += static_cast<unsigned long>(n);
            lastFrameMs = monoMs();
            if (cb_) {
                cb_(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n) * 2, user_);
            }
            continue;
        }
        if (monoMs() - lastFrameMs >= kSilenceMs) {
            ELOG("Mic", "no sound for %u ms: capture failed", static_cast<unsigned>(kSilenceMs));
            return false;
        }
        if (n == 0 || n == -EAGAIN) {
            const int ready = wait_(pcm, kWaitSliceMs);
            if (ready >= 0) {
                continue;  // a period is there, or the slice is over: look at the phone's request again
            }
            n = ready;  // an overrun or a suspend, reported by the wait
        }
        if (++errors <= 5) {
            ELOG("Mic", "read: %s", strerror_(static_cast<int>(n)));
        }
        if (recover_(pcm, static_cast<int>(n), 1) < 0) {
            return false;
        }
    }
    return true;
}

// One request of the phone, until it closes the mic or stop(). A failed capture is reopened after kReopenPauseMs;
// after kSilentFailuresBeforeRest failures in a row without a single frame, nothing more until the next request.
void EvoMic::listen() {
    unsigned silentFailures = 0;
    for (;;) {
        void* pcm = openCapture();
        if (!pcm) {
            restUntilAskedAgain();  // an open that fails is not retried until the phone asks again
            return;
        }
        ELOG("Mic", "open for the phone (%s)", kPcm);
        unsigned long frames = 0;
        unsigned errors = 0;
        const bool closed = captureUntilClosed(pcm, frames, errors);
        close_(pcm);
        ELOG("Mic", "closed (%lu ms captured, %u error(s))", frames / 16, errors);
        if (closed) {
            return;
        }
        silentFailures = frames ? 0 : silentFailures + 1;
        if (silentFailures >= kSilentFailuresBeforeRest) {
            ELOG("Mic", "%u captures in a row without sound: microphone left until the phone asks again",
                 silentFailures);
            restUntilAskedAgain();
            return;
        }
        if (!pauseWhileWanted(kReopenPauseMs)) {
            return;
        }
    }
}

void EvoMic::loop() {
    for (;;) {
        pthread_mutex_lock(&mu_);
        while (!quit_ && !want_) {
            pthread_cond_wait(&cv_, &mu_);
        }
        const bool quit = quit_;
        pthread_mutex_unlock(&mu_);
        if (quit) {
            return;
        }
        listen();
    }
}

}  // namespace evo
