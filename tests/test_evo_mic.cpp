// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car microphone (platform/evo_mic.cpp) on tests/fake_libasound.c: sound reaches the phone, and a silent or broken
// microphone neither holds stop() nor reopens in a loop.
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include "evo_log.hpp"
#include "evo_mic.hpp"

// _exit, not return: a microphone stuck in a read would also hold the stop() of EvoMic's destructor.
#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
            _exit(1);                                            \
        }                                                        \
    } while (0)

namespace {

// The fake microphone's behaviours (tests/fake_libasound.c).
const int kSound = 0, kSilent = 1, kBroken = 2;
const int kNonBlocking = 1;  // SND_PCM_NONBLOCK
// stop() must come back within kPromptMs; one still blocked after kStuckMs ends the test.
const uint32_t kPromptMs = 400, kStuckMs = 3000;

void (*behave)(int);
int (*opens)();
int (*openNow)();
int (*lastMode)();

volatile size_t g_bytes;
void onSound(const uint8_t*, size_t n, void*) {
    g_bytes = g_bytes + n;
}

void sleepMs(unsigned ms) {
    usleep(ms * 1000);
}

volatile bool g_stopDone;
void* watchStop(void*) {
    sleepMs(kStuckMs);
    if (!g_stopDone) {
        fprintf(stderr, "FAIL stop() still blocked after %u ms\n", static_cast<unsigned>(kStuckMs));
        _exit(1);
    }
    return nullptr;
}

// How long stop() took; the test ends if it never returns.
uint32_t timedStop(evo::EvoMic& mic) {
    g_stopDone = false;
    pthread_t watchdog;
    pthread_create(&watchdog, nullptr, watchStop, nullptr);
    pthread_detach(watchdog);
    const uint32_t from = evo::monoMs();
    mic.stop();
    g_stopDone = true;
    return evo::monoMs() - from;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test-evo-mic <fake-libasound.so>\n");
        return 2;
    }
    // Loaded first, under its soname: EvoMic's dlopen("libasound.so.2") then returns this library.
    void* fake = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    CHECK(fake);
    *reinterpret_cast<void**>(&behave) = dlsym(fake, "fake_mic_behave");
    *reinterpret_cast<void**>(&opens) = dlsym(fake, "fake_mic_opens");
    *reinterpret_cast<void**>(&openNow) = dlsym(fake, "fake_mic_open_now");
    *reinterpret_cast<void**>(&lastMode) = dlsym(fake, "fake_mic_last_mode");
    CHECK(behave && opens && openNow && lastMode);

    // A working microphone: opened non-blocking, its sound handed on, closed by stop().
    {
        behave(kSound);
        evo::EvoMic mic;
        CHECK(mic.start(onSound, nullptr));
        mic.request(true);
        sleepMs(300);
        CHECK(opens() == 1 && openNow() == 1 && (lastMode() & kNonBlocking));
        CHECK(g_bytes >= 2 * 320);  // at least one 20 ms read of 16-bit samples
        CHECK(timedStop(mic) < kPromptMs && openNow() == 0);
    }

    // A microphone that gives nothing: 2 s of silence fail the capture, opened again only after the 1 s pause, and
    // stop() returns at once.
    {
        behave(kSilent);
        evo::EvoMic mic;
        CHECK(mic.start(onSound, nullptr));
        mic.request(true);
        sleepMs(300);
        CHECK(opens() == 1 && openNow() == 1);
        sleepMs(2100);  // 2.4 s: the silent capture is closed, the pause runs
        CHECK(opens() == 1 && openNow() == 0);
        sleepMs(900);  // 3.3 s: opened again
        CHECK(opens() == 2 && openNow() == 1);
        CHECK(timedStop(mic) < kPromptMs && openNow() == 0);
    }

    // A microphone that fails as soon as it is opened: reopened once a second, three times in all, then left alone
    // until the phone's next request; stop() during a pause returns at once.
    {
        behave(kBroken);
        evo::EvoMic mic;
        CHECK(mic.start(onSound, nullptr));
        mic.request(true);
        sleepMs(400);
        CHECK(opens() == 1 && openNow() == 0);  // no reopen before the pause is over
        sleepMs(1100);
        CHECK(opens() == 2);
        sleepMs(1100);  // 2.6 s: the third failure in a row without sound
        CHECK(opens() == 3);
        sleepMs(900);  // 3.5 s: given up
        CHECK(opens() == 3 && openNow() == 0);
        mic.request(true);  // the phone's next request: a fresh start
        sleepMs(300);
        CHECK(opens() == 4 && openNow() == 0);
        CHECK(timedStop(mic) < kPromptMs);
    }
    puts(
        "PASS micro : ouvert sans blocage, son transmis, stop() immediat avec un micro muet ou en panne, 2 s sans son "
        "= echec, reouverture apres 1 s, abandon apres 3 echecs sans son jusqu'a la demande suivante");
    return 0;
}
