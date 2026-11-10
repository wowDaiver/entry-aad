// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto session on one phone connection: its thread, the channel handlers, and the car input for the phone.
#ifndef ENTRY_AAD_CORE_AA_SESSION_HPP
#define ENTRY_AAD_CORE_AA_SESSION_HPP

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "aa_frame.hpp"
#include "aa_input.hpp"
#include "aa_messages.hpp"
#include "aa_tls.hpp"
#include "aa_transport.hpp"
#include "car_inbox.hpp"
#include "car_location.hpp"

namespace aa {

// Callbacks run on the session thread, except where said: copy what is needed and return, never block.
// The session ended by itself (phone gone, silent, ByeBye); not after stop(). Possibly on the thread calling start().
using SessionEndedCallback = void (*)(void*);
// An H.264 access unit of the main display: media message count, data, phone timestamp.
using VideoFrameCallback = void (*)(uint32_t, const uint8_t*, size_t, uint64_t, void*);
// The H.264 codec configuration (SPS/PPS).
using CodecConfigCallback = void (*)(const uint8_t*, size_t, void*);
// The phone wants the screen (true) or gives it back to the car (false: its "exit to car" icon), and its reason.
using VideoFocusCallback = void (*)(bool, uint32_t, void*);
// The phone opens (Assistant, voice search) or closes the car microphone.
using MicRequestCallback = void (*)(bool, void*);
// Navigation status (channel 10, ids from 0x8000): the message id, then its body.
using NavStatusCallback = void (*)(uint16_t, const uint8_t*, size_t, void*);

class AASession {
public:
    AASession();
    ~AASession();

    // Takes the socket and starts the session thread; a session still running is ended first, and its ended
    // callback runs. False when the thread could not start: the socket is closed.
    bool start(int client_fd);
    // Takes ownership on every path. USB and TCP share TLS, channels and car callbacks.
    bool start(Transport* transport);
    // Ends the session and joins its thread. The ended callback does not run.
    void stop();
    bool isActive() const { return running(); }

    // Set before start().
    void setDisconnectCallback(SessionEndedCallback cb, void* user_data = nullptr);
    void setVideoFrameCallback(VideoFrameCallback cb, void* user_data = nullptr);
    void setCodecConfigCallback(CodecConfigCallback cb, void* user_data = nullptr);
    void setVideoFocusCallback(VideoFocusCallback cb, void* user_data = nullptr);
    void setMicRequestCallback(MicRequestCallback cb, void* user_data = nullptr);
    void setNavStatusCallback(NavStatusCallback cb, void* user_data = nullptr);
    void setTouchForwarding(bool on) { inbox_.setTouchForwarding(on); }  // on by default; keys go either way
    // The file holding the car's model and year (SdrIdentity::loadVehicleFile), read at each service discovery; kept
    // as given, so it must outlive the session. None by default: the default model and year.
    void setVehicleFile(const char* path) { vehicleFile_ = path; }
    // The program's version, announced to the phone as the head unit's software version; kept as given. None by
    // default: "0.0.0".
    void setSoftwareVersion(const char* version) { softwareVersion_ = version; }

    // Car input for the phone, from any thread: queued, then sent by the session thread.
    void postTouchEvent(const TouchEventOut& ev);
    void postKeyEvent(uint32_t keycode, bool down);       // Android key codes, as declared on the input channel
    void postRelativeEvent(uint32_t keycode, int delta);  // rotary steps (keycode 65536, ROTARY_CONTROLLER)
    // The car takes (false) or gives back (true) the screen: an unrequested video focus indication.
    void postVideoFocus(bool projected);
    // The car's day or night, kept across sessions: a NIGHT_MODE event once the phone started that sensor.
    void postNightMode(bool night);
    // The car's latest fix (monotonic capture time); an invalid one when the car's feed expired.
    void postCarLocation(const CarLocation& location);
    // Car microphone PCM, 16 kHz mono S16LE: sent while the phone keeps the microphone open, dropped otherwise.
    void postMicData(const uint8_t* pcm, size_t len);

private:
    template <typename Fn>
    struct Callback {
        Fn fn;
        void* user;
        Callback() : fn(nullptr), user(nullptr) {}
    };
    enum Received { kMessage, kNothing, kLost };

    // Lifecycle (aa_session.cpp). Only the session thread deletes the transport; start() and stop() cancel it to
    // wake that thread, then join it. socketLock_ keeps cancellation from racing with destruction.
    static void* threadMain(void* self);
    void run();
    bool running() const;
    void setRunning(bool on);
    bool clearRunning();  // the previous value
    void interrupt();
    void closeTransport();
    void finish();
    void resetCounters();

    // The wire (aa_session.cpp).
    bool receiveExactly(uint8_t* to, size_t size);  // also sends the car's input when it is posted
    bool sendExactly(const uint8_t* data, size_t size);
    bool sendFrame(uint8_t channel, uint8_t flags, const uint8_t* payload, size_t size);
    bool sendPlain(uint8_t channel, const Message& message);
    bool sendSealed(uint8_t channel, const Message& message, uint8_t flags = frame::kSealed);
    Received receive(uint8_t& channel, Message& message);

    // Car input (aa_session.cpp).
    void sendCarInput();
    bool sendLocation();
    bool sendSensorEvent(uint32_t type);
    bool sendVideoFocus(bool projected, bool unrequested);

    // One handler per route (aa_channels.cpp); dispatch() is false when the session must end.
    bool dispatch(uint8_t channel, const Message& message);
    bool onControl(uint16_t id, const Message& message);
    void onVersion(const Message& message);
    void onTlsHandshake(const Message& message);
    void onDiscovery();
    void onAudioFocus(const Message& message);
    void onNavigationFocus(const Message& message);
    void onByeBye(const Message& message);
    void onPing(const Message& message);
    void onChannelOpen(uint8_t channel);
    void onSensorStart(uint8_t channel, const Message& message);
    void onSetup(uint8_t channel);
    void onStart(uint8_t channel, const Message& message);
    void onMedia(uint8_t channel, uint16_t id, const Message& message);
    void onVideoFocusRequest(uint8_t channel, const Message& message);
    void onInputBinding(uint8_t channel);
    void onMicrophoneOpen(uint8_t channel, const Message& message);
    void onNavigationStatus(uint16_t id, const Message& message);
    void onPlaybackStatus(uint16_t id, const Message& message);

    pthread_mutex_t lifecycleLock_;  // start() and stop(), called from the accept and main threads
    mutable pthread_mutex_t socketLock_;
    Transport* transport_;  // guarded by socketLock_; I/O only on session thread
    bool running_;          // guarded by socketLock_
    pthread_t thread_;

    Callback<SessionEndedCallback> ended_;
    Callback<VideoFrameCallback> video_;
    Callback<CodecConfigCallback> codec_;
    Callback<VideoFocusCallback> focus_;
    Callback<MicRequestCallback> microphone_;
    Callback<NavStatusCallback> navigation_;
    const char* vehicleFile_;
    const char* softwareVersion_;

    CarInbox inbox_;
    // Session thread only.
    TlsClient tls_;
    MessageAssembler assembler_;
    bool ioFailed_;               // session thread only; a partial failed send must end the byte stream
    bool videoReady_;             // the phone set the video channel up: a focus can go
    uint32_t mediaSessions_[32];  // per channel, the session id of its start indication, echoed in our acks
    uint32_t mediaMessages_;      // media messages received, every channel
    uint32_t audioMessages_[2];   // guidance (8), media (9)
    uint32_t microphoneChunks_;   // sent since the phone opened the microphone
    // Log throttles, per session.
    unsigned framesSeen_[256];
    unsigned keysLogged_, movesLogged_, navigationLogged_, pings_;
    uint32_t playbackState_;
};

}  // namespace aa

#endif  // ENTRY_AAD_CORE_AA_SESSION_HPP
