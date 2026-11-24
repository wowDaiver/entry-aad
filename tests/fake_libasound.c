// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* PC-only stand-in for the HU's libasound.so.2 (tests/test_evo_mic.cpp): the calls platform/evo_mic.cpp makes, on a
 * microphone the test chooses with fake_mic_behave(), and counters it reads back. Built with the soname libasound.so.2:
 * once the test has loaded it, EvoMic's dlopen("libasound.so.2") gets this library, never the PC's own. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { kSound = 0, kSilent = 1, kBroken = 2 };
enum { kNonBlocking = 1, kFramesPerMs = 16, kPeriodFrames = 320 };

struct fake_pcm {
    int mode;
    int started;
    long long startMs;
    long long consumed;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_behaviour = kSound;
static int g_opens, g_openNow, g_lastMode = -1;

static long long nowMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void sleepMs(long long ms) {
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&t, 0);
}
static int behaviour(void) {
    pthread_mutex_lock(&g_mu);
    const int b = g_behaviour;
    pthread_mutex_unlock(&g_mu);
    return b;
}
/* Frames captured and not read yet: 16 per ms since the first read started the capture, as with ALSA's start
 * threshold of one frame. */
static long long pending(const struct fake_pcm* p) {
    return p->started ? (nowMs() - p->startMs) * kFramesPerMs - p->consumed : 0;
}

/* Controls and counters for the test: a behaviour, and the open count started again from zero. */
void fake_mic_behave(int how) {
    pthread_mutex_lock(&g_mu);
    g_behaviour = how;
    g_opens = 0;
    pthread_mutex_unlock(&g_mu);
}
int fake_mic_opens(void) {
    pthread_mutex_lock(&g_mu);
    const int n = g_opens;
    pthread_mutex_unlock(&g_mu);
    return n;
}
int fake_mic_open_now(void) {
    pthread_mutex_lock(&g_mu);
    const int n = g_openNow;
    pthread_mutex_unlock(&g_mu);
    return n;
}
int fake_mic_last_mode(void) {
    pthread_mutex_lock(&g_mu);
    const int n = g_lastMode;
    pthread_mutex_unlock(&g_mu);
    return n;
}

/* The libasound calls, with the PC's own signatures reduced to what EvoMic passes. */
int snd_pcm_open(void** pcm, const char* name, int stream, int mode) {
    (void)name;
    (void)stream;
    struct fake_pcm* p = calloc(1, sizeof(*p));
    if (!p) {
        return -ENOMEM;
    }
    p->mode = mode;
    pthread_mutex_lock(&g_mu);
    ++g_opens;
    ++g_openNow;
    g_lastMode = mode;
    pthread_mutex_unlock(&g_mu);
    *pcm = p;
    return 0;
}
int snd_pcm_set_params(void* pcm, int format, int access, unsigned channels, unsigned rate, int resample,
                       unsigned latency) {
    (void)pcm;
    (void)format;
    (void)access;
    (void)channels;
    (void)rate;
    (void)resample;
    (void)latency;
    return 0;
}
long snd_pcm_readi(void* pcm, void* buffer, unsigned long size) {
    struct fake_pcm* p = pcm;
    const int how = behaviour();
    if (how == kBroken) {
        return -EIO;
    }
    if (how == kSilent) {
        if (p->mode & kNonBlocking) {
            return -EAGAIN;
        }
        for (;;) {
            pause(); /* a blocking read on a microphone that gives nothing never returns */
        }
    }
    if (!p->started) {
        p->started = 1;
        p->startMs = nowMs();
    }
    while (pending(p) < (long long)size && !(p->mode & kNonBlocking)) {
        sleepMs(5);
    }
    long long n = pending(p);
    if (n <= 0) {
        return -EAGAIN;
    }
    if (n > (long long)size) {
        n = (long long)size;
    }
    memset(buffer, 0, (size_t)n * 2);
    p->consumed += n;
    return (long)n;
}
int snd_pcm_wait(void* pcm, int timeout) {
    struct fake_pcm* p = pcm;
    const long long until = nowMs() + timeout;
    /* Only a running capture with sound wakes the wait before its timeout. */
    while (nowMs() < until) {
        if (behaviour() == kSound && p->started && pending(p) >= kPeriodFrames) {
            return 1;
        }
        sleepMs(5);
    }
    return 0;
}
int snd_pcm_recover(void* pcm, int err, int silent) {
    (void)pcm;
    (void)silent;
    return behaviour() == kBroken ? err : 0;
}
int snd_pcm_close(void* pcm) {
    free(pcm);
    pthread_mutex_lock(&g_mu);
    --g_openNow;
    pthread_mutex_unlock(&g_mu);
    return 0;
}
const char* snd_strerror(int err) {
    return err == -EIO ? "fake I/O error" : "fake error";
}
