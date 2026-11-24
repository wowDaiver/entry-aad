// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// iDrive controller, keys and volume (MOST, read through vMOST) to Android Auto.

// Observed on the car: FBlock 0xE5 (the controller) is shadowed by HmiMain3D (instances 03 and 05) and NavStartup
// (01); one pair can be shadowed by dozens of clients. We register as a shadow too (a copy of the controller's Status
// messages): nothing is ever sent to the controller and iDrive keeps receiving everything.
// libvmostclient.so.1 is loaded at run time (platform/vmost_client.hpp) and kept until the process exits.
// Decoding: platform/evo_ccs.hpp (instance 03, functions C46/C47/C4F; older controllers: instance 01, C10/C20).
//
// A shadow registration alone receives no message: vmostd.conf sets EnableFunctionBasedFiltering=true, and vmostd
// only forwards a status message (OpType > 8) to a client once bit (FBlock + low byte of the function) & 0xFF of that
// client's filter is set. vmostd sets that bit, before routing, when the client sends a request (OpType <= 8) on that
// function. So we send a plain read (Get, OpType 1) of C46/C47/C4F to E5.03: no Notification.Set/Clear, no
// controller setting changed; iDrive receives the status reply too.
//
// A car with the older controller (a BMW i3: its only E5 message was E5.01 function C30, let through by the filter
// bit the steering wheel phone button's read had set) never answers those reads, and its C10/C20 status messages
// stayed filtered out. When CCS35up stays silent, the same plain read is sent for C10 and C20 on E5.01
// (aa::CcsProtocol, evo_ccs.hpp); a car that answers on CCS35up is sent nothing more.
//
// The connection is lent to one other user, the F-series cluster's guidance fields (kombi_most.hpp, VmostPeer): its
// messages go to it, it ticks on this thread, and it says when it has a call to send, so the wait may be shorter.
#ifndef ENTRY_AAD_PLATFORM_EVO_KNOB_HPP
#define ENTRY_AAD_PLATFORM_EVO_KNOB_HPP

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "evo_ccs.hpp"
#include "input_mode.hpp"
#include "vmost_client.hpp"
#include "volume_state.hpp"

namespace evo {

// Controller keys as with CarPlay (configureHardKeys, evo_knob.cpp).
void configureHardKeys(aa::CcsDecoder& d);
// Android Auto keys that open one of its apps (AAK_TEL / AAK_NAVIGATION / AAK_MEDIA).
bool isAppKey(uint32_t keycode);
// Keys sent to Android Auto that iDrive also acts on underneath, Android Auto keeping the screen: the app keys (iDrive
// opens its own navigation, media or phone screen), BACK and tilt left (iDrive walks back its history, to our menu or
// to the screen an app key opened: seen on an F30, Android Auto then stepped aside, #43).
bool keepsAndroidAuto(uint32_t keycode);

// CCS35up PushButtonsState (C47/C4F), one byte per key, as HmiMain3D reads it (CDevCCS35up::pushbuttonsToKeycodeAction
// and getButtonActivity): 0 released, 1 pressed, 2/3 long press, 4 finger resting on the touch-sensitive key (iDrive:
// "not pressed"), 7 no information. The core decoder takes any non-zero byte as a press, so merely touching a key
// fired it (observed on the car). Normalised before the decoder: 1/2/3 -> 1, 0/4 -> 0, any other value -> the key's
// previous normalised value. prev holds that state (n <= 8).
void normalizePushButtons(const uint8_t* in, size_t n, uint8_t* prev, uint8_t* out);

// Steering wheel telephone button, ControlElementsMFL 0x15.02 (libhmil6services CPxyControlElementsMFL::init:
// 0x400 TelephoneButton, 0x401 AudioVolumeButton, 0x511 SearchSwitch1, 0x512 PTTButton1, 0x513 SpecialFunction).
// Status = 1 byte: 0 released, 1 pressed, 3 no information. Returns 1 / 0, or -1 otherwise.
const uint16_t kMflTelephoneFkt = 0x400;
int decodeTelephoneButton(const VmostMsgView& m);

// Volume buttons, as HmiMain3D's CDevVolumeControl reads them (libhmil6services):
//   CECenterStack       MOST 0x15 instance 0x0C fkt 0xC01 AudioVolumeButtonCS, Status: AudioVolume(0..3), Steps
//   ControlElementsMFL  MOST 0x15 instance 0x02 fkt 0x401 AudioVolumeButton,   Status: UWord, AudioVolume, Steps
// AudioVolume 1 / 2 = one direction each (HMI table 03 02 01), 0 and 3 = nothing. Returns 1 for a volume step.
const uint8_t kVolumeFblock = 0x15, kVolumeCsInstance = 0x0C, kVolumeMflInstance = 0x02;
const uint16_t kVolumeCsFkt = 0xC01, kVolumeMflFkt = 0x401;
int decodeVolumeButton(const VmostMsgView& m, unsigned* steps);

// Volume level, read where HmiMain3D's own volume bar reads it: CHmiVolumeIndicatorService watches CPxyAudioMaster
// instance 0, whose init binds fkt 0xC08 (libhmil6services). Array property (volume_state.hpp): Status = PosX, PosY,
// then records of value, min, max, one byte each (e.g. 01 00 LEVEL 00 MAX). Only the front display's record, the
// first one (PosX 0: the whole array, its first record; PosX 1: that record alone). Our copy needs vmostd's filter
// opened by one Get, as for E5: nothing is ever set on the AudioMaster.
const uint8_t kAudioMasterFblock = 0x20, kAudioMasterInstance = 0x00;
const uint16_t kVolumeLevelFkt = 0xC08;
bool decodeVolumeLevel(const VmostMsgView& m, VolumeLevel& out);

// The audio knob's push button (CECenterStack 0x15.0C, fkt 0xC04 PushButtonAudio, bound by CPxyCECenterStack::init):
// Status = one enum byte, 1 pressed. iDrive mutes or unmutes on the press. Returns 1 for a press.
const uint16_t kAudioButtonFkt = 0xC04;
int decodeAudioButton(const VmostMsgView& m);

class EvoKnob : private VmostPort {
public:
    using Callback = void (*)(const aa::CcsEvent&, void*);
    EvoKnob();
    ~EvoKnob();
    bool start(InputMode mode, Callback cb, void* user);
    // The car's coding, read by the iDrive app (controller_layout.hpp): which key each button is. Before start().
    void setCoding(const aa::ControllerCoding& coding);
    // The other user of the connection (the cluster's MOST guidance fields). Before start().
    void setPeer(VmostPeer* peer) { peer_ = peer; }
    void setOwned(bool owned);  // keys are forwarded only while the Android Auto picture shows
    using VolumeCallback = void (*)(unsigned int, void*);
    void setVolumeCallback(VolumeCallback cb, void* user) {
        volCb_ = cb;
        volUser_ = user;
    }
    // Hard key left to iDrive (keycode 0 after configureHardKeys), on key-down: name = MENU, COM, MEDIA, NAV...
    using HardKeyCallback = void (*)(const char*, void*);
    void setHardKeyCallback(HardKeyCallback cb, void* user) {
        keyCb_ = cb;
        keyUser_ = user;
    }
    // A key sent to Android Auto that iDrive also acts on (keepsAndroidAuto: NAV, MEDIA, COM, BACK, tilt left, and the
    // steering wheel phone), on key-down: iDrive switches screens too (it gets every key), so the caller keeps Android
    // Auto up and brings iDrive back.
    void setAppKeyCallback(HardKeyCallback cb, void* user) {
        appKeyCb_ = cb;
        appKeyUser_ = user;
    }
    // Raw MEDIA state even while Android Auto is hidden; 7 cancels a gesture after another input.
    using MediaCallback = void (*)(unsigned, void*);
    void setMediaCallback(MediaCallback cb, void* user) {
        mediaCb_ = cb;
        mediaUser_ = user;
    }
    // Each volume level the car reports (any source: knob, steering wheel, iDrive). Called on the vMOST thread.
    using VolumeLevelCallback = void (*)(const VolumeLevel&, void*);
    void setVolumeLevelCallback(VolumeLevelCallback cb, void* user) {
        levelCb_ = cb;
        levelUser_ = user;
    }
    // The audio knob pressed (iDrive mutes or unmutes). Called on the vMOST thread.
    using AudioButtonCallback = void (*)(void*);
    void setAudioButtonCallback(AudioButtonCallback cb, void* user) {
        audioButtonCb_ = cb;
        audioButtonUser_ = user;
    }
    // During a phone call (iDrive's call screen) COM and the steering wheel phone button stay with iDrive.
    void setPhoneToIdrive(bool v) { phoneToIdrive_ = v; }
    void stop();
    void summary() const;
    void onMessage(const VmostMsgView& m);  // called from the vMOST callback, on our thread
    // PC tests only: decoding/forwarding without vmostd (start() is never called).
    void attachForTest(InputMode mode, Callback cb, void* user) {
        mode_ = mode;
        cb_ = cb;
        user_ = user;
    }
    // PC tests only: what the knob thread does each poll turn while the picture is hidden.
    void pollTurnForTest() {
        if (!owned_) {
            releaseHeldKeys("picture gone");
        }
    }
    aa::CcsProtocol::Kind protocol() const { return protocol_.kind(); }

private:
    // VmostPort, for the peer
    int send(VmostMsgOut& m) override;
    int shadow(uint8_t fblock, uint8_t inst, bool on) override;
    void pump(int ms) override;

    static void* threadMain(void* self);
    void run();
    bool load();
    // Reads C10 and C20 on E5.01 once, which opens vmostd's filter for the older controller protocol.
    void readOlderProtocol();
    InputMode mode_;
    Callback cb_;
    void* user_;
    pthread_t thread_;
    bool started_;
    std::atomic<bool> running_, owned_;
    VmostClient vmost_;
    bool connected_;
    int fd_;
    VmostPeer* peer_;
    unsigned gets_;
    uint8_t registered_[4];
    int nRegistered_;
    uint8_t volRegistered_[2];
    int nVolRegistered_;
    VolumeCallback volCb_;
    VolumeLevelCallback levelCb_;
    void* levelUser_;
    AudioButtonCallback audioButtonCb_;
    void* audioButtonUser_;
    bool levelRegistered_;
    unsigned levels_;
    HardKeyCallback keyCb_;
    void* keyUser_;
    void* volUser_;
    HardKeyCallback appKeyCb_;
    void* appKeyUser_;
    MediaCallback mediaCb_;
    void* mediaUser_;
    std::atomic<bool> phoneToIdrive_;
    bool telDown_;
    uint8_t pushPrev_[8];
    unsigned volumeSteps_;
    aa::CcsDecoder decoder_;
    aa::CcsProtocol protocol_;  // knob thread only
    bool otherProtocolLogged_;
    void observeMedia(const VmostMsgView& m);
    void forwardKey(uint32_t keycode, const char* what);
    // Keys whose press reached Android Auto: their release always follows, even once the picture is gone (camera,
    // popup, driving mode, menu), or the phone would keep them down. Knob thread only.
    void releaseHeldKeys(const char* why);
    bool takeHeld(uint32_t keycode);
    uint32_t held_[8];
    int nHeld_;
    unsigned messages_, e5_, events_, sent_, rawLogged_;
};

}  // namespace evo

#endif
