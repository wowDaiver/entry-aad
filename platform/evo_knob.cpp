// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// iDrive controller, keys and volume read through vMOST (see evo_knob.hpp).
#include "evo_knob.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "evo_log.hpp"
#include "media_recall.hpp"

namespace evo {

static const uint8_t kControllerFblock = 0xE5;
static const uint8_t kInstances[] = {0x01, 0x03, 0x05};  // NavStartup 01, HmiMain3D 03 and 05 (seen on the car)

bool decodeVolumeLevel(const VmostMsgView& m, VolumeLevel& out) {
    if (m.fblock != kAudioMasterFblock || m.fkt != kVolumeLevelFkt || m.op != 0xC || !m.data || m.len < 5) {
        return false;
    }
    if (m.data[0] > 1 || m.data[1] != 0) {  // another display's record, or not a whole record
        return false;
    }
    const unsigned value = m.data[2], min = m.data[3], max = m.data[4];
    if (max <= min || value < min || value > max) {
        return false;
    }
    out.value = value;
    out.min = min;
    out.max = max;
    return true;
}

static const uint16_t kCcs35upFunctions[] = {0xC46, 0xC47, 0xC4F};  // RotaryEncoderState, PushButtonsState(2)

int decodeAudioButton(const VmostMsgView& m) {
    return m.fblock == kVolumeFblock && m.inst == kVolumeCsInstance && m.fkt == kAudioButtonFkt && m.op == 0xC &&
                   m.data && m.len >= 1 && m.data[0] == 1
               ? 1
               : 0;
}

int decodeVolumeButton(const VmostMsgView& m, unsigned* steps) {
    if (m.fblock != kVolumeFblock || m.op != 0xC || !m.data) {
        return 0;
    }
    unsigned raw = 0, n = 0;
    if (m.inst == kVolumeCsInstance && m.fkt == kVolumeCsFkt && m.len >= 2) {
        raw = m.data[0];
        n = m.data[1];
    } else if (m.inst == kVolumeMflInstance && m.fkt == kVolumeMflFkt && m.len >= 4) {
        raw = m.data[2];
        n = m.data[3];
    } else {
        return 0;
    }
    if (raw != 1 && raw != 2) {
        return 0;
    }
    if (steps) {
        *steps = n;
    }
    return 1;
}

void normalizePushButtons(const uint8_t* in, size_t n, uint8_t* prev, uint8_t* out) {
    for (size_t i = 0; i < n && i < 8; ++i) {
        const uint8_t v = in[i];
        if (v == 1 || v == 2 || v == 3) {
            prev[i] = 1;  // pressed / long press
        } else if (v == 0 || v == 4) {
            prev[i] = 0;  // released / finger resting on the touch key
        }
        out[i] = prev[i];  // 7 (no information) and unknown: unchanged
    }
}

static int decodeMflButton(const VmostMsgView& m, uint16_t fkt) {
    if (m.fblock != kVolumeFblock || m.inst != kVolumeMflInstance || m.fkt != fkt || m.op != 0xC || !m.data ||
        m.len < 1) {
        return -1;
    }
    return m.data[0] == 1 ? 1 : m.data[0] == 0 ? 0 : -1;
}
int decodeTelephoneButton(const VmostMsgView& m) {
    return decodeMflButton(m, kMflTelephoneFkt);
}

bool isAppKey(uint32_t k) {
    return k == aa::AAK_TEL || k == aa::AAK_NAVIGATION || k == aa::AAK_MEDIA;
}

bool keepsAndroidAuto(uint32_t k) {
    return isAppKey(k) || k == aa::AAK_BACK || k == aa::AAK_DPAD_LEFT;
}

static void vmostCallback(void* msg, void* user) {
    VmostMsgView v;
    if (user && parseVmostMsg(msg, v)) {
        static_cast<EvoKnob*>(user)->onMessage(v);
    }
}

EvoKnob::EvoKnob()
    : mode_(INPUT_OFF),
      cb_(nullptr),
      user_(nullptr),
      started_(false),
      running_(false),
      owned_(false),
      connected_(false),
      fd_(-1),
      peer_(nullptr),
      gets_(0),
      nRegistered_(0),
      nVolRegistered_(0),
      volCb_(nullptr),
      levelCb_(nullptr),
      levelUser_(nullptr),
      audioButtonCb_(nullptr),
      audioButtonUser_(nullptr),
      levelRegistered_(false),
      levels_(0),
      keyCb_(nullptr),
      keyUser_(nullptr),
      volUser_(nullptr),
      appKeyCb_(nullptr),
      appKeyUser_(nullptr),
      mediaCb_(nullptr),
      mediaUser_(nullptr),
      phoneToIdrive_(false),
      telDown_(false),
      volumeSteps_(0),
      otherProtocolLogged_(false),
      messages_(0),
      e5_(0),
      events_(0),
      sent_(0),
      rawLogged_(0) {
    nHeld_ = 0;
    memset(held_, 0, sizeof(held_));
    memset(pushPrev_, 0, sizeof(pushPrev_));
    configureHardKeys(decoder_);
}

// Controller keys while Android Auto is on screen, as iDrive does for CarPlay (CCarPlayActivity::handleControllerInput:
// NAV and the map key -> CarPlay "maps:", COM -> "mobilephone:", MEDIA -> CarPlay, MENU -> leave CarPlay).
//   NAV / MAP / MEDIA / COM  Android Auto's navigation (both NAV and MAP) / media / phone app (AA keys NAVIGATION
//                      65538, MEDIA 65537, TEL 65540; not KEYCODE_CALL, which made the phone redial on the car). iDrive
//                      switches screens too (it gets every key): the service keeps Android Auto up and the iDrive app
//                      brings iDrive back at once.
//   MENU               iDrive (Android Auto steps aside). OPTION: the Android Auto menu of the iDrive app.
//   BACK               Android Auto's back.
void configureHardKeys(aa::CcsDecoder& d) {
    static const char* const local[] = {"MENU", "OPTIONS"};
    for (size_t i = 0; i < sizeof(local) / sizeof(local[0]); ++i) {
        d.setSlotKey(aa::CcsDecoder::slotByName(local[i]), 0);
    }
    d.setSlotKey(aa::CcsDecoder::slotByName("COM"), aa::AAK_TEL);
    d.setSlotKey(aa::CcsDecoder::slotByName("NAV"), aa::AAK_NAVIGATION);
    d.setSlotKey(aa::CcsDecoder::slotByName("MAP"), aa::AAK_NAVIGATION);  // #1
    d.setSlotKey(aa::CcsDecoder::slotByName("MEDIA"), aa::AAK_MEDIA);
}

void EvoKnob::setCoding(const aa::ControllerCoding& coding) {
    decoder_.setCoding(coding);
    ELOG("Knob",
         "controller keys as iDrive reads them on this car: %s controller (variant 0xb80 %s); older protocol "
         "0x08 = %s, 0x20 = %s, 0x40 = %s",
         coding.gStyle ? "G-style" : "F-style", coding.gStyle ? "set" : "not set",
         aa::CcsDecoder::slotName(decoder_.sensorSlot(0x08)), aa::CcsDecoder::slotName(decoder_.sensorSlot(0x20)),
         aa::CcsDecoder::slotName(decoder_.sensorSlot(0x40)));
}

EvoKnob::~EvoKnob() {
    stop();
}

bool EvoKnob::load() {
    switch (vmost_.load()) {
        case VmostClient::kLoaded:
            return true;
        case VmostClient::kNoDaemon:
            ELOG("Knob", "vmostd socket missing: controller not read");
            return false;
        case VmostClient::kNoLibrary:
            ELOG("Knob", "libvmostclient.so.1: %s", vmost_.error().c_str());
            return false;
        case VmostClient::kNoSymbols:
            ELOG("Knob", "vMOST functions missing: controller not read");
            return false;
    }
    return false;
}

bool EvoKnob::start(InputMode mode, Callback cb, void* user) {
    if (started_ || mode == INPUT_OFF) {
        return mode == INPUT_OFF;
    }
    mode_ = mode;
    cb_ = cb;
    user_ = user;
    if (!load()) {
        return false;
    }
    running_ = true;
    if (pthread_create(&thread_, nullptr, threadMain, this) != 0) {
        running_ = false;
        ELOG("Knob", "cannot start the thread");
        return false;
    }
    started_ = true;
    return true;
}

void EvoKnob::setOwned(bool owned) {
    owned_ = owned;
}

void EvoKnob::stop() {
    if (!started_) {
        return;
    }
    running_ = false;
    pthread_join(thread_, nullptr);
    started_ = false;
    // The library keeps static state and GLib objects: it stays loaded until the process exits.
}

void EvoKnob::summary() const {
    ELOG("Knob",
         "summary: mode=%s vmost_messages=%u e5_messages=%u events=%u sent_to_aa=%u registrations=%d "
         "filter_opening_reads=%u volume_levels=%u",
         inputModeName(mode_), messages_, e5_, events_, sent_, nRegistered_, gets_, levels_);
}

void* EvoKnob::threadMain(void* self) {
    static_cast<EvoKnob*>(self)->run();
    return nullptr;
}

// One key press + release to Android Auto (steering wheel phone button: one MOST status per change).
void EvoKnob::forwardKey(uint32_t keycode, const char* what) {
    aa::CcsEvent e;
    e.kind = 1;
    e.keycode = keycode;
    e.delta = 0;
    e.what = what;
    e.down = 1;
    cb_(e, user_);
    e.down = 0;
    cb_(e, user_);
    sent_ += 2;
}

bool EvoKnob::takeHeld(uint32_t keycode) {
    for (int i = 0; i < nHeld_; ++i) {
        if (held_[i] == keycode) {
            held_[i] = held_[--nHeld_];
            return true;
        }
    }
    return false;
}

void EvoKnob::releaseHeldKeys(const char* why) {
    while (nHeld_ > 0 && cb_) {
        aa::CcsEvent e;
        e.kind = 1;
        e.keycode = held_[--nHeld_];
        e.down = 0;
        e.delta = 0;
        e.what = "RELEASE";
        ELOG("Knob", "key %u released to AA (%s)", e.keycode, why);
        cb_(e, user_);
        ++sent_;
    }
}

void EvoKnob::observeMedia(const VmostMsgView& m) {
    if (!mediaCb_ || mode_ != INPUT_FORWARD || m.op != 0xC || !m.data) {
        return;
    }
    // Which buttons are MEDIA follows the car's coding (decoder_.setCoding): on a small F controller, its media key.
    if (m.inst == 3 && (m.fkt == 0xC47 || m.fkt == 0xC4F) && (m.len == 7 || m.len == 8)) {
        int state = -1;
        for (int i = 0; i < 7; ++i) {
            const bool pressed = m.data[i] >= 1 && m.data[i] <= 3;
            if (decoder_.byteSlot(i) != aa::kSlotMedia) {
                if (pressed) {
                    mediaCb_(MediaDoublePress::kOtherInput, mediaUser_);
                    return;
                }
            } else if (state < 0 || pressed) {
                state = m.data[i];
            }
        }
        if (state >= 0) {
            mediaCb_(static_cast<unsigned>(state), mediaUser_);
        }
    } else if (m.inst == 1 && m.fkt == 0xC20 && m.len >= 7 && m.data[2] != 0xFF && m.data[3] <= 2 && m.data[5] == 0 &&
               decoder_.sensorSlot(m.data[6]) == aa::kSlotMedia) {
        // One message per edge, never a status: before a press, the key was released. Without this, the first press
        // after the service started only taught the detector the key's state.
        if (m.data[3] == 1) {
            mediaCb_(0, mediaUser_);
        }
        mediaCb_(m.data[3], mediaUser_);
    }
}

void EvoKnob::onMessage(const VmostMsgView& m) {
    ++messages_;
    if (peer_ && peer_->wants(m)) {
        peer_->onMessage(m);
        return;
    }
    if (m.fblock == kAudioMasterFblock) {  // volume level (copy of what the AudioMaster tells HmiMain3D)
        VolumeLevel level;
        if (decodeVolumeLevel(m, level)) {
            if (++levels_ <= 50) {
                ELOG("Knob", "volume level: %u (%u to %u)", level.value, level.min, level.max);
            }
            if (levelCb_) {
                levelCb_(level, levelUser_);
            }
        } else if (levels_ < 50 && m.fkt == kVolumeLevelFkt) {
            ELOG("Knob", "volume level message not recognised: operation 0x%x, %u byte(s), first 0x%02x", m.op, m.len,
                 m.data && m.len ? m.data[0] : 0);
        }
        return;
    }
    if (m.fblock == kVolumeFblock) {  // volume knob / steering wheel buttons (copies)
        unsigned steps = 0;
        if (decodeVolumeButton(m, &steps)) {
            if (++volumeSteps_ <= 200) {
                ELOG("Knob", "volume %s: %u step(s)", m.inst == kVolumeCsInstance ? "centre console" : "steering wheel",
                     steps);
            }
            if (volCb_) {
                volCb_(steps, volUser_);
            }
        }
        if (decodeAudioButton(m)) {
            ELOG("Knob", "audio knob pressed (iDrive mutes or unmutes)");
            if (audioButtonCb_) {
                audioButtonCb_(audioButtonUser_);
            }
        }
        const int tel = decodeTelephoneButton(m);
        if (tel == 1 && !telDown_) {
            telDown_ = true;
            const bool send = mode_ == INPUT_FORWARD && owned_ && cb_ && !phoneToIdrive_;
            ELOG("Knob", "steering wheel phone button: %s",
                 send             ? "-> Android Auto (phone)"
                 : phoneToIdrive_ ? "call in progress: left to iDrive"
                                  : "Android Auto not on screen: iDrive");
            if (send) {
                forwardKey(aa::AAK_TEL, "WHEEL_PHONE");
                if (appKeyCb_) {
                    appKeyCb_("WHEEL_PHONE", appKeyUser_);
                }
            }
        } else if (tel == 0) {
            telDown_ = false;
        }
        // The steering wheel voice button stays with iDrive: it does not start Android Auto's Assistant.
        return;
    }
    if (m.fblock != kControllerFblock) {
        return;
    }
    ++e5_;
    const unsigned rawMax = mode_ == INPUT_OBSERVE ? 400 : 40;
    if (rawLogged_ < rawMax) {
        ++rawLogged_;
        char hex[3 * 48 + 1];
        size_t o = 0;
        for (size_t i = 0; i < m.len && i < 48; ++i) {
            o += snprintf(hex + o, sizeof(hex) - o, "%02x ", m.data[i]);
        }
        hex[o] = 0;
        ELOG("Knob", "raw addr=0x%04x E5.%02x fkt=0x%03x op=0x%x len=%u: %s", m.addr, m.inst, m.fkt, (unsigned)m.op,
             m.len, hex);
    }
    if (m.op == 0xC) {  // one protocol per car: the first one heard
        const bool chosen = protocol_.kind() != aa::CcsProtocol::kUnknown;
        if (!protocol_.accept(m.inst, m.fkt)) {
            if (!otherProtocolLogged_) {
                otherProtocolLogged_ = true;
                ELOG("Knob", "E5.%02x function 0x%03x left out: this car's controller protocol is %s", m.inst, m.fkt,
                     aa::CcsProtocol::name(protocol_.kind()));
            }
            return;
        }
        if (!chosen && protocol_.kind() != aa::CcsProtocol::kUnknown) {
            ELOG("Knob", "controller protocol: %s", aa::CcsProtocol::name(protocol_.kind()));
        }
    }
    const uint8_t* data = m.data;
    observeMedia(m);
    uint8_t keys[8];
    if (m.inst == 0x03 && (m.fkt == 0xC47 || m.fkt == 0xC4F) && m.op == 0xC && m.data && m.len <= sizeof(keys)) {
        // Match CcsDecoder's front-controller instance before updating retained state (status 7).
        normalizePushButtons(m.data, m.len, pushPrev_, keys);  // touch (4) is not a press
        data = keys;
    }
    aa::CcsEvent ev[8];
    const int n = decoder_.feed(m.fblock, m.inst, m.fkt, static_cast<uint8_t>(m.op), data, m.len, ev, 8);
    const bool send = mode_ == INPUT_FORWARD && owned_ && cb_;
    for (int i = 0; i < n; ++i) {
        if (mediaCb_ && mode_ == INPUT_FORWARD &&
            (ev[i].kind == 2 || (ev[i].kind == 1 && ev[i].down && ev[i].keycode != aa::AAK_MEDIA))) {
            mediaCb_(MediaDoublePress::kOtherInput, mediaUser_);
        }
        const char* what = ev[i].what ? ev[i].what : "?";
        // A release whose press reached Android Auto follows it there, before anything else is decided: COM pressed
        // just before a call started must come up on the phone, not on iDrive's call screen.
        const bool releaseToPhone = ev[i].kind == 1 && !ev[i].down && takeHeld(ev[i].keycode);
        // During a call, COM stays with iDrive's call screen (CarPlay does the same: ICallManager check).
        const bool localKey = !releaseToPhone && ev[i].kind == 1 &&
                              (ev[i].keycode == 0 || (ev[i].keycode == aa::AAK_TEL && phoneToIdrive_));
        if (localKey) {  // hard key left to iDrive (configureHardKeys)
            if (ev[i].down) {
                ELOG("Knob", "key %s left to iDrive (not sent to AA)", what);
                if (keyCb_) {
                    keyCb_(what, keyUser_);
                }
            }
            continue;
        }
        if (ev[i].kind == 1 && keepsAndroidAuto(ev[i].keycode) && ev[i].down && send && appKeyCb_) {
            appKeyCb_(what, appKeyUser_);
        }
        // A press goes to Android Auto only while it owns the screen; a release goes wherever its press went.
        bool forward = send;
        if (ev[i].kind == 1) {
            if (!ev[i].down) {
                forward = releaseToPhone;
            } else if (send) {
                takeHeld(ev[i].keycode);  // pressed again without a release: still held once
                if (nHeld_ < static_cast<int>(sizeof(held_) / sizeof(held_[0]))) {
                    held_[nHeld_++] = ev[i].keycode;
                }
            }
        }
        ++events_;
        if (events_ <= 400) {
            ELOG("Knob", "%s %s code=%u down=%d delta=%d", forward ? "-> AA" : "decoded (not sent)",
                 ev[i].what ? ev[i].what : "?", ev[i].keycode, ev[i].down, ev[i].delta);
        }
        if (forward) {
            cb_(ev[i], user_);
            ++sent_;
        }
    }
}

void EvoKnob::readOlderProtocol() {
    static const uint8_t dummy[4] = {0, 0, 0, 0};
    static const uint16_t functions[] = {0xC10, 0xC20};  // RotaryEncoder, PushButton
    ELOG("Knob", "no CCS35up status %d ms after the reads: reading the older controller protocol too",
         static_cast<int>(aa::CcsProtocol::kSilenceMs));
    for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i) {
        VmostMsgOut m;
        buildVmostGet(m, kControllerFblock, aa::CcsDecoder::kCcsInstance, functions[i], dummy);
        const int rc = vmost_.send(&m);
        if (rc == 0) {
            ++gets_;
        }
        ELOG("Knob", "read (Get) E5.%02x function 0x%03x: %s (%d); it opens vmostd's filter for that function",
             static_cast<unsigned>(aa::CcsDecoder::kCcsInstance), functions[i], rc == 0 ? "sent" : "refused", rc);
    }
}

int EvoKnob::send(VmostMsgOut& m) {
    return vmost_.send(&m);
}

int EvoKnob::shadow(uint8_t fblock, uint8_t inst, bool on) {
    return on ? vmost_.shadowRegister(fblock, inst) : vmost_.shadowDeregister(fblock, inst);
}

// Only while the reader closes (VmostPeer::finish): the answers to the last calls.
void EvoKnob::pump(int ms) {
    const uint32_t until = monoMs() + static_cast<uint32_t>(ms > 0 ? ms : 0);
    for (;;) {
        const int32_t left = static_cast<int32_t>(until - monoMs());
        if (left <= 0 || fd_ < 0) {
            return;
        }
        struct pollfd p;
        p.fd = fd_;
        p.events = POLLIN;
        p.revents = 0;
        const int r = poll(&p, 1, left);
        if (r <= 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        if (vmost_.processMessages() != 0) {
            return;
        }
    }
}

void EvoKnob::run() {
    int rc = vmost_.connect(vmostCallback, this, 0);
    if (rc != 0) {
        ELOG("Knob", "vmostd refused the connection (%d)", rc);
        return;
    }
    connected_ = true;
    for (size_t i = 0; i < sizeof(kInstances); ++i) {
        rc = vmost_.shadowRegister(kControllerFblock, kInstances[i]);
        ELOG("Knob", "shadow registration E5.%02x: %s (%d)", kInstances[i], rc == 0 ? "OK" : "refused", rc);
        if (rc == 0) {
            registered_[nRegistered_++] = kInstances[i];
        }
    }
    // Open our vmostd function filter for the CCS35up properties: a plain Get of each (read-only request).
    static const uint8_t dummy[4] = {0, 0, 0, 0};
    for (size_t i = 0; nRegistered_ && i < sizeof(kCcs35upFunctions) / sizeof(kCcs35upFunctions[0]); ++i) {
        VmostMsgOut m;
        buildVmostGet(m, kControllerFblock, 0x03, kCcs35upFunctions[i], dummy);
        rc = vmost_.send(&m);
        if (rc == 0) {
            ++gets_;
        }
        ELOG("Knob", "read (Get) E5.03 function 0x%03x: %s (%d); it opens vmostd's filter for that function",
             kCcs35upFunctions[i], rc == 0 ? "sent" : "refused", rc);
    }
    if (nRegistered_) {
        protocol_.ccs35upAsked(monoMs());
    }
    int fd = -1;
    if (!nRegistered_ || vmost_.fileDescriptorGet(&fd) != 0 || fd < 0) {
        ELOG("Knob", "no usable registration: controller not read");
    } else {
        ELOG("Knob", "copy of the iDrive controller's messages on; mode %s", inputModeName(mode_));
        // Volume buttons: same passive copy + read-only Get that opens vmostd's filter (as for E5).
        static const uint8_t volInst[2] = {kVolumeCsInstance, kVolumeMflInstance};
        static const uint16_t volFkt[2] = {kVolumeCsFkt, kVolumeMflFkt};
        for (int i = 0; i < 2; ++i) {
            int vr = vmost_.shadowRegister(kVolumeFblock, volInst[i]);
            if (vr == 0) {
                volRegistered_[nVolRegistered_++] = volInst[i];
            }
            VmostMsgOut vm;
            buildVmostGet(vm, kVolumeFblock, volInst[i], volFkt[i], dummy);
            int gr = vr == 0 ? vmost_.send(&vm) : -1;
            ELOG("Knob", "volume %s 15.%02x function 0x%03x: shadow %s (%d), read (Get) %s (%d)",
                 i == 0 ? "centre console" : "steering wheel", volInst[i], volFkt[i], vr == 0 ? "OK" : "refused", vr,
                 gr == 0 ? "sent" : "not sent", gr);
            if (volInst[i] == kVolumeCsInstance && vr == 0) {  // the audio knob's push button, same instance
                VmostMsgOut bm;
                buildVmostGet(bm, kVolumeFblock, kVolumeCsInstance, kAudioButtonFkt, dummy);
                const int br = vmost_.send(&bm);
                ELOG("Knob", "audio knob button 15.%02x function 0x%03x: read (Get) %s (%d)", kVolumeCsInstance,
                     kAudioButtonFkt, br == 0 ? "sent" : "not sent", br);
            }
            if (volInst[i] == kVolumeMflInstance && vr == 0) {  // steering wheel phone button, same instance
                VmostMsgOut tm;
                buildVmostGet(tm, kVolumeFblock, kVolumeMflInstance, kMflTelephoneFkt, dummy);
                int tr = vmost_.send(&tm);
                ELOG("Knob", "steering wheel phone button 15.%02x function 0x%03x: read (Get) %s (%d)",
                     kVolumeMflInstance, kMflTelephoneFkt, tr == 0 ? "sent" : "not sent", tr);
            }
        }
        // Volume level: same passive copy; the Get (record PosX 1, PosY 0, read-only) opens vmostd's filter for it.
        {
            const int lr = vmost_.shadowRegister(kAudioMasterFblock, kAudioMasterInstance);
            levelRegistered_ = lr == 0;
            static const uint8_t record[2] = {0x01, 0x00};
            VmostMsgOut lm;
            buildVmostGet(lm, kAudioMasterFblock, kAudioMasterInstance, kVolumeLevelFkt, record, sizeof(record));
            const int gr = levelRegistered_ ? vmost_.send(&lm) : -1;
            ELOG("Knob", "volume level 20.%02x function 0x%03x: shadow %s (%d), read (Get) %s (%d)",
                 kAudioMasterInstance, kVolumeLevelFkt, levelRegistered_ ? "OK" : "refused", lr,
                 gr == 0 ? "sent" : "not sent", gr);
        }
        fd_ = fd;
        while (running_) {
            struct pollfd p;
            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            int wait = 100;
            if (peer_) {
                const int due = peer_->dueInMs(monoMs());
                wait = due < wait ? due : wait;
            }
            int r = poll(&p, 1, wait);
            if (r < 0 && errno != EINTR) {
                ELOG("Knob", "poll: %s", strerror(errno));
                break;
            }
            if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                ELOG("Knob", "vmostd closed the connection");
                break;
            }
            if (r > 0 && (p.revents & POLLIN)) {
                rc = vmost_.processMessages();
                if (rc != 0) {
                    ELOG("Knob", "vMOST reading interrupted (%d)", rc);
                    connected_ = false;
                    break;
                }
            }
            if (!owned_) {
                releaseHeldKeys("picture gone");  // within 100 ms of the picture leaving
            }
            if (protocol_.olderDue(monoMs())) {
                readOlderProtocol();
            }
            if (peer_) {
                peer_->tick(monoMs(), *this);
            }
        }
        if (peer_ && connected_) {
            peer_->finish(*this);
        }
        fd_ = -1;
    }
    releaseHeldKeys("controller reading stopped");
    if (connected_) {
        if (levelRegistered_) {
            vmost_.shadowDeregister(kAudioMasterFblock, kAudioMasterInstance);
            levelRegistered_ = false;
        }
        for (int i = 0; i < nVolRegistered_; ++i) {
            vmost_.shadowDeregister(kVolumeFblock, volRegistered_[i]);
        }
        nVolRegistered_ = 0;
        for (int i = 0; i < nRegistered_; ++i) {
            rc = vmost_.shadowDeregister(kControllerFblock, registered_[i]);
            ELOG("Knob", "deregistration E5.%02x: %s (%d)", registered_[i], rc == 0 ? "OK" : "failed", rc);
        }
        vmost_.disconnect();
        connected_ = false;
    }
}

}  // namespace evo
