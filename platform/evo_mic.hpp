// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Car microphone for the Android Auto Assistant (AA mic channel 14, 16 kHz mono S16LE).
#ifndef ENTRY_AAD_PLATFORM_EVO_MIC_HPP
#define ENTRY_AAD_PLATFORM_EVO_MIC_HPP
// Reads the HU's shared capture PCM "mic1" (/etc/asound.conf: dsnoop ipc_key 1025 on the Dirana capture, channel 0,
// shared with natp, so no conflict with BMW telephony), through the HU's own libasound.so.2 loaded with dlopen.
// Read-only on the car: nothing is sent on vMOST/MOST. Samples go to the phone as read.
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace evo {

class EvoMic {
public:
    using DataCallback = void (*)(const uint8_t*, size_t, void*);
    EvoMic();
    ~EvoMic();
    // Starts the worker thread (idle until request(true)). False when libasound or one of its functions is missing,
    // or when the thread cannot be created.
    bool start(DataCallback cb, void* user);
    // Phone opened/closed the mic. Never blocks (called on the AA session thread).
    void request(bool open);
    // Returns within about a wait slice (100 ms), whatever the microphone does.
    void stop();

private:
    static void* threadStub(void* arg);
    void loop();
    bool load();
    void listen();
    void* openCapture();
    bool captureUntilClosed(void* pcm, unsigned long& frames, unsigned& errors);
    bool wanted();
    bool pauseWhileWanted(unsigned ms);
    void restUntilAskedAgain();
    DataCallback cb_;
    void* user_;
    void* lib_;
    pthread_t thread_;
    pthread_mutex_t mu_;
    pthread_cond_t cv_;
    bool started_, quit_, want_;
    unsigned asks_;  // open requests from the phone, counted
    // libasound entry points (resolved with dlsym)
    int (*open_)(void**, const char*, int, int);
    int (*setParams_)(void*, int, int, unsigned, unsigned, int, unsigned);
    long (*readi_)(void*, void*, unsigned long);
    int (*wait_)(void*, int);
    int (*recover_)(void*, int, int);
    int (*close_)(void*);
    const char* (*strerror_)(int);
};

}  // namespace evo

#endif  // ENTRY_AAD_PLATFORM_EVO_MIC_HPP
