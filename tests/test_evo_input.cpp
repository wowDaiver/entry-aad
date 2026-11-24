// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Inputs: IPv4/UDP tap parsing, touch state machine, vMOST messages to the controller decoder, keys and volume.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_knob.hpp"
#include "evo_touch.hpp"
#include "media_recall.hpp"
#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)

static size_t ipUdp(uint8_t* p, const uint8_t* payload, size_t n, uint16_t sport, uint16_t dport) {
    memset(p, 0, 28);
    const size_t total = 28 + n;
    p[0] = 0x45;
    p[2] = (uint8_t)(total >> 8);
    p[3] = (uint8_t)total;
    p[6] = 0x40;
    p[8] = 64;
    p[9] = 17;
    p[12] = 160;
    p[13] = 48;
    p[14] = 199;
    p[15] = 115;  // source = touch controller
    p[16] = 160;
    p[17] = 48;
    p[18] = 199;
    p[19] = 119;
    p[20] = (uint8_t)(sport >> 8);
    p[21] = (uint8_t)sport;
    p[22] = (uint8_t)(dport >> 8);
    p[23] = (uint8_t)dport;
    p[24] = (uint8_t)((n + 8) >> 8);
    p[25] = (uint8_t)(n + 8);
    memcpy(p + 28, payload, n);
    return total;
}

static int frame(evo::TouchTracker& t, int x, int y, int state, unsigned char seq, aa::TouchEventOut* ev) {
    unsigned char f[112];
    aa::TouchContact c;
    c.id = 0;
    c.state = state;
    c.x = x;
    c.y = y;
    c.size = 10;
    aa::buildTouchFrame(f, &c, 1, 0, seq);
    return t.feed(f, sizeof(f), ev, 16);
}

int main() {
    evo::InputMode mode;
    CHECK(evo::parseInputMode(nullptr, mode) && mode == evo::INPUT_OFF);
    CHECK(evo::parseInputMode("off", mode) && mode == evo::INPUT_OFF);
    CHECK(evo::parseInputMode("forward", mode) && mode == evo::INPUT_FORWARD);
    CHECK(evo::parseInputMode("observe", mode) && mode == evo::INPUT_OBSERVE);
    CHECK(!evo::parseInputMode("Forward", mode));

    // IPv4/UDP view as delivered by AF_PACKET SOCK_DGRAM (network header first).
    uint8_t pdu[112];
    aa::TouchContact c0;
    c0.id = 0;
    c0.state = 1;
    c0.x = 640;
    c0.y = 240;
    c0.size = 9;
    aa::buildTouchFrame(pdu, &c0, 1, 0, 0);
    uint8_t pkt[256];
    evo::UdpView u;
    size_t n = ipUdp(pkt, pdu, sizeof(pdu), 30501, 30567);
    CHECK(evo::parseIpv4Udp(pkt, n, u) && u.dport == 30567 && u.sport == 30501 && u.len == 112);
    CHECK(u.src == ((160u << 24) | (48u << 16) | (199u << 8) | 115u) && evo::isTouchPdu(u.data, u.len));
    CHECK(!evo::parseIpv4Udp(pkt, n - 1, u));  // truncated
    pkt[6] = 0x20;                             // more fragments
    CHECK(!evo::parseIpv4Udp(pkt, n, u));
    pkt[6] = 0x40;
    pkt[7] = 0x01;  // fragment offset
    CHECK(!evo::parseIpv4Udp(pkt, n, u));
    pkt[7] = 0x00;
    pkt[9] = 6;  // TCP
    CHECK(!evo::parseIpv4Udp(pkt, n, u));
    pkt[9] = 17;
    pkt[0] = 0x65;  // not IPv4
    CHECK(!evo::parseIpv4Udp(pkt, n, u));
    pkt[0] = 0x45;
    CHECK(evo::parseIpv4Udp(pkt, n, u));

    // Touch state machine: one tap = PRESS, DRAG, RELEASE; a duplicate is never a second tap.
    evo::TouchTracker t;
    aa::TouchEventOut ev[16];
    CHECK(frame(t, 100, 200, 1, 0, ev) == 1 && ev[0].action == 0 && ev[0].n == 1 && ev[0].p[0].x == 100 &&
          ev[0].p[0].y == 200);
    CHECK(frame(t, 100, 200, 1, 0, ev) == 0);  // same message again
    CHECK(frame(t, 110, 210, 2, 12, ev) == 1 && ev[0].action == 2 && ev[0].p[0].x == 110 && ev[0].p[0].y == 210);
    CHECK(frame(t, 112, 212, 1, 24, ev) == 1 && ev[0].action == 2);  // repeated down, nothing lost: a move
    CHECK(frame(t, 112, 212, 3, 36, ev) == 1 && ev[0].action == 1 && t.activeCount() == 0);
    CHECK(frame(t, 1279, 479, 1, 48, ev) == 1 && ev[0].action == 0 && ev[0].p[0].x == 1279 && ev[0].p[0].y == 479);
    CHECK(t.releaseAll(ev, 16) == 1 && ev[0].action == 1 && t.activeCount() == 0);  // lost "up"
    CHECK(t.feed(pkt, 20, ev, 16) == -1);                                           // not a touch PDU
    // A panel that sets bits 6-7 of the state byte (iDrive keeps them apart, bits 4-5 are the state): its drag is a
    // press, moves and a release, not a tap. Before: states 5, 6, 7 were dropped and the finger looked lifted (#30).
    evo::TouchTracker f45;
    CHECK(frame(f45, 300, 200, 0x5, 0, ev) == 1 && ev[0].action == 0 && ev[0].p[0].x == 300);      // 01 01: down
    CHECK(frame(f45, 340, 200, 0x6, 12, ev) == 1 && ev[0].action == 2 && ev[0].p[0].x == 340);     // 01 10: move
    CHECK(frame(f45, 380, 205, 0xA, 24, ev) == 1 && ev[0].action == 2 && ev[0].p[0].x == 380);     // 10 10: move
    CHECK(f45.activeCount() == 1);                                                                 // still down
    CHECK(frame(f45, 380, 205, 0x7, 36, ev) == 1 && ev[0].action == 1 && f45.activeCount() == 0);  // 01 11: up
    {
        unsigned char raw[112];
        aa::TouchContact c = {2, 0xE, 640, 240, 9};  // id 2, bits 6-7 = 11, state 2
        aa::buildTouchFrame(raw, &c, 1, 0, 0);
        aa::TouchReassembler r;
        aa::TouchMsg msg[aa::kTouchMaxMsgs];
        aa::TouchContact got[2];
        int declared = 0;
        CHECK(r.feed(raw, sizeof(raw), msg, aa::kTouchMaxMsgs) == 1 &&
              aa::parseTouchStream(msg[0], got, 2, &declared) == 1 && declared == 1 && got[0].state == 2 &&
              got[0].id == 2 && got[0].x == 640 && got[0].y == 240);
    }
    // A MINI: the phone's display starts 40 columns into the panel (its left margin) and is 1200x480. A finger is sent
    // in the phone's pixels: 40 less, kept inside its display. Before, it landed 40 pixels right of it (#78, #88).
    evo::TouchTracker mini;
    mini.setOffset(-40, 0);
    mini.setLimits(1199, 479);
    CHECK(frame(mini, 100, 200, 1, 0, ev) == 1 && ev[0].action == 0 && ev[0].p[0].x == 60 && ev[0].p[0].y == 200);
    CHECK(frame(mini, 20, 210, 2, 12, ev) == 1 && ev[0].p[0].x == 0);  // under the surround: the edge
    CHECK(frame(mini, 1270, 479, 2, 24, ev) == 1 && ev[0].p[0].x == 1199 && ev[0].p[0].y == 479);
    CHECK(frame(mini, 1270, 479, 3, 36, ev) == 1 && ev[0].action == 1 && mini.activeCount() == 0);
    // Before the picture: a finger that touches down is ignored until it lifts, even once the picture is up.
    evo::TouchTracker early;
    early.setPictureStarted(false);
    CHECK(frame(early, 100, 300, 1, 0, ev) == 0);  // no press without a picture
    CHECK(frame(early, 110, 300, 2, 12, ev) == 0);
    early.setPictureStarted(true);
    CHECK(frame(early, 120, 300, 2, 24, ev) == 0);  // the picture starting cannot turn this finger into a tap
    CHECK(frame(early, 120, 300, 3, 36, ev) == 0);
    CHECK(frame(early, 100, 400, 1, 48, ev) == 1 && ev[0].action == 0);  // the next touch goes through
    early.setPictureStarted(false);  // picture gone mid-gesture: released where it was
    CHECK(frame(early, 100, 410, 2, 60, ev) == 1 && ev[0].action == 1 && ev[0].p[0].y == 400 &&
          early.activeCount() == 0);
    // The picture steps aside (volume, popup, reverse, MENU) with a finger down on the phone, and its lift is decoded
    // before the touch thread's next turn: the lift still goes to the phone, which would otherwise keep the finger
    // down. Nothing else goes once the picture has left: no new touch, no drag, no lift of a finger it never saw.
    {
        evo::TouchTracker tr;
        evo::PhoneFingers phone;
        CHECK(frame(tr, 200, 100, 1, 0, ev) == 1 && phone.admit(ev[0], true) && phone.any());
        CHECK(frame(tr, 210, 100, 2, 12, ev) == 1 && phone.admit(ev[0], true));
        CHECK(frame(tr, 210, 100, 3, 24, ev) == 1 && ev[0].action == 1);
        CHECK(phone.admit(ev[0], false) && !phone.any());  // the picture has gone: this lift goes all the same
        CHECK(frame(tr, 300, 100, 1, 36, ev) == 1 && !phone.admit(ev[0], false));  // a new touch is iDrive's
        CHECK(frame(tr, 300, 100, 3, 48, ev) == 1 && !phone.admit(ev[0], false) && !phone.any());  // so is its lift
        CHECK(frame(tr, 400, 100, 1, 60, ev) == 1 && phone.admit(ev[0], true));  // the picture is back
        CHECK(frame(tr, 420, 100, 2, 72, ev) == 1 && ev[0].action == 2 && !phone.admit(ev[0], false));
        // Two fingers: the phone saw the first one go down, not the second one, which came after the picture left.
        aa::TouchContact two[2] = {{0, 2, 420, 100, 9}, {1, 1, 600, 300, 9}};
        unsigned char f2[112];
        aa::buildTouchFrame(f2, two, 2, 0, 84);
        CHECK(tr.feed(f2, sizeof(f2), ev, 16) == 1 && ev[0].action == 5 && !phone.admit(ev[0], false));
        aa::buildTouchFrame(f2, two, 1, 0, 96);  // the second finger lifts
        CHECK(tr.feed(f2, sizeof(f2), ev, 16) == 1 && ev[0].action == 6 && !phone.admit(ev[0], false));
        two[0].state = 3;  // then the first one
        aa::buildTouchFrame(f2, two, 1, 0, 108);
        CHECK(tr.feed(f2, sizeof(f2), ev, 16) == 1 && ev[0].action == 1 && phone.admit(ev[0], false) && !phone.any());
    }
    // The same message split over three datagrams.
    evo::TouchTracker s;
    unsigned char whole[112], parts[3 * 112];
    int sizes[3];
    const int per[3] = {2, 5, 5};
    aa::TouchContact c1;
    c1.id = 1;
    c1.state = 1;
    c1.x = 300;
    c1.y = 100;
    c1.size = 5;
    aa::buildTouchFrame(whole, &c1, 1, 0, 0);
    CHECK(aa::splitTouchFrame(whole, per, 3, parts, sizes) == 3);
    CHECK(s.feed(parts, sizes[0], ev, 16) == 0 && s.feed(parts + 112, sizes[1], ev, 16) == 0);
    CHECK(s.feed(parts + 224, sizes[2], ev, 16) == 1 && ev[0].action == 0 && ev[0].p[0].id == 1 && ev[0].p[0].x == 300);
    aa::TouchContact last[5];
    CHECK(s.lastContacts(last, 5) == 1 && last[0].y == 100 && s.messages() == 1);

    // vMOST message as handed to the client callback, then the CCS35up decoder (instance 03).
    uint8_t d1[8] = {1, 0x00, 100, 0, 0, 0, 0, 0xff}, d2[8] = {2, 0x00, 102, 1, 0, 0, 0, 0xff};
    unsigned char msg[64];
    memset(msg, 0, sizeof(msg));
    uint16_t addr = 0x0123, fkt = 0x0C46, len = 8;
    uint32_t op = 0xC;
    const uint8_t* dp = d1;
    memcpy(msg, &addr, 2);
    msg[2] = 0xE5;
    msg[3] = 0x03;
    memcpy(msg + 4, &fkt, 2);
    memcpy(msg + 8, &op, 4);
    memcpy(msg + 12, &len, 2);
    memcpy(msg + 16, &dp, sizeof(dp));
    evo::VmostMsgView v;
    CHECK(evo::parseVmostMsg(msg, v) && v.addr == 0x0123 && v.fblock == 0xE5 && v.inst == 3 && v.fkt == 0xC46 &&
          v.op == 0xC && v.len == 8 && v.data == d1);
    aa::CcsDecoder dec;
    aa::CcsEvent ce[8];
    CHECK(dec.feed(v.fblock, v.inst, v.fkt, (uint8_t)v.op, v.data, v.len, ce, 8) == 0);  // first position: baseline
    dp = d2;
    memcpy(msg + 16, &dp, sizeof(dp));
    CHECK(evo::parseVmostMsg(msg, v));
    int k = dec.feed(v.fblock, v.inst, v.fkt, (uint8_t)v.op, v.data, v.len, ce, 8);
    CHECK(k == 2 && ce[0].kind == 2 && ce[0].keycode == aa::AAK_ROTARY_CONTROLLER && ce[0].delta == 2);
    CHECK(ce[1].kind == 1 && ce[1].keycode == aa::AAK_DPAD_CENTER && ce[1].down == 1);
    CHECK(!evo::parseVmostMsg(0, v));
    op = 0x10;
    memcpy(msg + 8, &op, 4);
    CHECK(!evo::parseVmostMsg(msg, v));
    op = 0xC;
    memcpy(msg + 8, &op, 4);
    len = 5000;
    memcpy(msg + 12, &len, 2);
    CHECK(!evo::parseVmostMsg(msg, v));
    len = 8;
    memcpy(msg + 12, &len, 2);
    dp = 0;
    memcpy(msg + 16, &dp, sizeof(dp));
    CHECK(!evo::parseVmostMsg(msg, v));
    evo::VmostMsgOut g;
    evo::buildVmostGet(g, 0xE5, 0x03, 0xC4F, d1);
    CHECK(g.addr == 0xFFFF && g.fblock == 0xE5 && g.inst == 3 && g.fkt == 0xC4F && g.op == 1 && g.len == 0 &&
          g.data == d1 && g.channel == 0 && g.reserved == 0);
    CHECK(evo::parseVmostMsg(&g, v) && v.op == 1 && v.fkt == 0xC4F);
    // Controller keys as with CarPlay. PushButtonsState 0xC47, one byte per slot: MENU BACK OPTIONS COM MEDIA NAV MAP.
    {
        aa::CcsDecoder hk;
        evo::configureHardKeys(hk);
        CHECK(hk.slotKey(aa::CcsDecoder::slotByName("MENU")) == 0 &&
              hk.slotKey(aa::CcsDecoder::slotByName("OPTIONS")) == 0);               // iDrive's
        CHECK(hk.slotKey(aa::CcsDecoder::slotByName("MAP")) == aa::AAK_NAVIGATION);  // as CarPlay's "maps:" (#1)
        CHECK(hk.slotKey(aa::CcsDecoder::slotByName("COM")) == aa::AAK_TEL);  // not KEYCODE_CALL (phone redialled)
        CHECK(hk.slotKey(aa::CcsDecoder::slotByName("NAV")) == aa::AAK_NAVIGATION);
        CHECK(hk.slotKey(aa::CcsDecoder::slotByName("MEDIA")) == aa::AAK_MEDIA);
        CHECK(hk.slotKey(aa::CcsDecoder::slotByName("BACK")) == aa::AAK_BACK);
        CHECK(evo::isAppKey(aa::AAK_TEL) && evo::isAppKey(aa::AAK_NAVIGATION) && evo::isAppKey(aa::AAK_MEDIA) &&
              !evo::isAppKey(aa::AAK_BACK) && !evo::isAppKey(0) && !evo::isAppKey(aa::AAK_CALL));
        // BACK and tilt left also make iDrive move underneath (#43): Android Auto keeps the screen for them too.
        CHECK(evo::keepsAndroidAuto(aa::AAK_TEL) && evo::keepsAndroidAuto(aa::AAK_NAVIGATION) &&
              evo::keepsAndroidAuto(aa::AAK_MEDIA) && evo::keepsAndroidAuto(aa::AAK_BACK) &&
              evo::keepsAndroidAuto(aa::AAK_DPAD_LEFT));
        CHECK(!evo::keepsAndroidAuto(0) && !evo::keepsAndroidAuto(aa::AAK_DPAD_RIGHT) &&
              !evo::keepsAndroidAuto(aa::AAK_DPAD_UP) && !evo::keepsAndroidAuto(aa::AAK_DPAD_DOWN) &&
              !evo::keepsAndroidAuto(aa::AAK_DPAD_CENTER) && !evo::keepsAndroidAuto(aa::AAK_CALL));
        setenv("EVO_KEY_COM", "0", 1);  // the environment is never read
        aa::CcsDecoder ov;
        evo::configureHardKeys(ov);
        CHECK(ov.slotKey(aa::CcsDecoder::slotByName("COM")) == aa::AAK_TEL);
        unsetenv("EVO_KEY_COM");
        // Touch-sensitive keys (observed on the car: merely touching OPTION left Android Auto): 4 = finger resting =
        // not pressed, 7 = no information (state kept), 1/2/3 = pressed.
        uint8_t prev[8] = {0}, out[8];
        const uint8_t touch[7] = {0, 0, 4, 0, 0, 0, 0}, press[7] = {0, 0, 1, 0, 0, 0, 0},
                      noinf[7] = {7, 7, 7, 7, 7, 7, 7};
        const uint8_t hold[7] = {0, 0, 2, 0, 0, 0, 0}, rel[7] = {0, 0, 4, 0, 0, 0, 0};
        evo::normalizePushButtons(touch, 7, prev, out);
        CHECK(out[2] == 0);
        evo::normalizePushButtons(press, 7, prev, out);
        CHECK(out[2] == 1);
        evo::normalizePushButtons(noinf, 7, prev, out);
        CHECK(out[2] == 1 && out[0] == 0);
        evo::normalizePushButtons(hold, 7, prev, out);
        CHECK(out[2] == 1);
        evo::normalizePushButtons(rel, 7, prev, out);
        CHECK(out[2] == 0);  // finger still on it after the press
        // Through EvoKnob (Android Auto on screen): touches never act, presses do; app keys reach Android Auto.
        struct Rec {
            static void ev(const aa::CcsEvent& e, void* u) {
                Rec* r = static_cast<Rec*>(u);
                if (r->n < 16) {
                    r->e[r->n++] = e;
                }
            }
            static void local(const char* name, void* u) {
                Rec* r = static_cast<Rec*>(u);
                snprintf(r->localKey, sizeof(r->localKey), "%s", name);
                ++r->locals;
            }
            static void app(const char* name, void* u) {
                Rec* r = static_cast<Rec*>(u);
                snprintf(r->appKey, sizeof(r->appKey), "%s", name);
                ++r->apps;
            }
            aa::CcsEvent e[16];
            int n, locals, apps;
            char localKey[16], appKey[24];
        } rec;
        memset(&rec, 0, sizeof(rec));
        evo::EvoKnob kk;
        kk.attachForTest(evo::INPUT_FORWARD, &Rec::ev, &rec);
        kk.setOwned(true);
        kk.setHardKeyCallback(&Rec::local, &rec);
        kk.setAppKeyCallback(&Rec::app, &rec);
        evo::VmostMsgView pv;
        pv.addr = 0x0100;
        pv.fblock = 0xE5;
        pv.inst = 0x03;
        pv.fkt = 0xC47;
        pv.op = 0xC;
        pv.len = 7;
        uint8_t k0[7] = {0}, kOptTouch[7] = {0, 0, 4, 0, 0, 0, 0}, kOpt[7] = {0, 0, 1, 0, 0, 0, 0};
        uint8_t kNavTouch[7] = {0, 0, 0, 0, 0, 4, 0}, kNav[7] = {0, 0, 0, 0, 0, 1, 0}, kCom[7] = {0, 0, 0, 1, 0, 0, 0};
        pv.data = k0;
        kk.onMessage(pv);
        pv.data = kOptTouch;
        kk.onMessage(pv);
        CHECK(rec.locals == 0 && rec.n == 0);  // touch: nothing
        pv.data = kOpt;
        kk.onMessage(pv);
        CHECK(rec.locals == 1 && !strcmp(rec.localKey, "OPTIONS"));
        pv.data = kOptTouch;
        kk.onMessage(pv);
        pv.data = k0;
        kk.onMessage(pv);
        pv.data = kNavTouch;
        kk.onMessage(pv);
        CHECK(rec.n == 0 && rec.apps == 0);
        pv.data = kNav;
        kk.onMessage(pv);
        CHECK(rec.n == 1 && rec.e[0].keycode == aa::AAK_NAVIGATION && rec.e[0].down == 1 && rec.apps == 1 &&
              !strcmp(rec.appKey, "NAV"));
        pv.data = k0;
        kk.onMessage(pv);
        CHECK(rec.n == 2 && rec.e[1].down == 0);
        kk.setPhoneToIdrive(true);  // call: COM is iDrive's call screen
        pv.data = kCom;
        kk.onMessage(pv);
        CHECK(rec.n == 2 && rec.locals == 2 && !strcmp(rec.localKey, "COM"));
        pv.data = k0;
        kk.onMessage(pv);
        kk.setPhoneToIdrive(false);
        pv.data = kCom;
        kk.onMessage(pv);
        CHECK(rec.n == 3 && rec.e[2].keycode == aa::AAK_TEL && rec.apps == 2);
        pv.data = k0;
        kk.onMessage(pv);
        // BACK goes to Android Auto and is reported too: iDrive may walk back to the screen NAV opened.
        uint8_t kBack[7] = {0, 1, 0, 0, 0, 0, 0};
        pv.data = kBack;
        kk.onMessage(pv);
        CHECK(rec.n == 5 && rec.e[4].keycode == aa::AAK_BACK && rec.e[4].down == 1 && rec.apps == 3 &&
              !strcmp(rec.appKey, "BACK"));
        pv.data = k0;
        kk.onMessage(pv);
        CHECK(rec.n == 6 && rec.e[5].down == 0 && rec.apps == 3);  // the release is not reported
        // A press that reached Android Auto gets its release, once, even if the picture left meanwhile.
        {
            const int beforeHold = rec.n, appsBefore = rec.apps;
            pv.data = kNav;
            kk.onMessage(pv);
            CHECK(rec.n == beforeHold + 1 && rec.e[beforeHold].down == 1);
            kk.setOwned(false);  // camera, popup or menu takes the screen while NAV is held
            kk.pollTurnForTest();
            CHECK(rec.n == beforeHold + 2 && rec.e[beforeHold + 1].down == 0 &&
                  rec.e[beforeHold + 1].keycode == aa::AAK_NAVIGATION);
            kk.pollTurnForTest();
            pv.data = k0;
            kk.onMessage(pv);  // the real release: already sent
            CHECK(rec.n == beforeHold + 2);
            pv.data = kNav;
            kk.onMessage(pv);  // a press while iDrive owns the screen: its release does not go to AA either
            pv.data = k0;
            kk.onMessage(pv);
            CHECK(rec.n == beforeHold + 2);
            kk.setOwned(true);
            rec.n = beforeHold;  // the recorder keeps 16 events
            rec.apps = appsBefore;
        }
        // COM held when a call starts: its press went to Android Auto, so its release goes there too instead of
        // iDrive's call screen, or the phone keeps the key down. A COM pressed during the call stays iDrive's.
        {
            const int beforeCall = rec.n, appsBefore = rec.apps, localsBefore = rec.locals;
            pv.data = kCom;
            kk.onMessage(pv);
            CHECK(rec.n == beforeCall + 1 && rec.e[beforeCall].keycode == aa::AAK_TEL && rec.e[beforeCall].down == 1);
            kk.setPhoneToIdrive(true);  // the call starts while COM is held
            pv.data = k0;
            kk.onMessage(pv);
            CHECK(rec.n == beforeCall + 2 && rec.e[beforeCall + 1].keycode == aa::AAK_TEL &&
                  rec.e[beforeCall + 1].down == 0 && rec.locals == localsBefore);
            kk.setOwned(false);
            kk.pollTurnForTest();  // nothing is left held: no second release when the picture goes
            CHECK(rec.n == beforeCall + 2);
            kk.setOwned(true);
            pv.data = kCom;
            kk.onMessage(pv);
            pv.data = k0;
            kk.onMessage(pv);
            CHECK(rec.n == beforeCall + 2 && rec.locals == localsBefore + 1 && !strcmp(rec.localKey, "COM"));
            kk.setPhoneToIdrive(false);
            rec.n = beforeCall;
            rec.apps = appsBefore;
            rec.locals = localsBefore;
        }
        // An ignored controller instance must not change the state retained for status 7 (no information).
        const int beforeForeign = rec.n;
        pv.inst = 5;
        pv.data = kNav;
        kk.onMessage(pv);
        pv.inst = 3;
        pv.data = noinf;
        kk.onMessage(pv);
        CHECK(rec.n == beforeForeign);
        pv.data = k0;
        kk.onMessage(pv);
        // Steering wheel phone button 15.02.400: 1 pressed / 0 released / 3 no information -> AA phone app.
        evo::VmostMsgView tv;
        tv.addr = 0x0100;
        tv.fblock = 0x15;
        tv.inst = 0x02;
        tv.fkt = 0x400;
        tv.op = 0xC;
        tv.len = 1;
        uint8_t t1[1] = {1}, t0[1] = {0}, t3[1] = {3};
        tv.data = t1;
        CHECK(evo::decodeTelephoneButton(tv) == 1);
        tv.data = t0;
        CHECK(evo::decodeTelephoneButton(tv) == 0);
        tv.data = t3;
        CHECK(evo::decodeTelephoneButton(tv) == -1);
        tv.fkt = 0x401;
        tv.data = t1;
        CHECK(evo::decodeTelephoneButton(tv) == -1);
        tv.fkt = 0x400;
        const int before = rec.n;
        tv.data = t1;
        kk.onMessage(tv);
        tv.data = t3;
        kk.onMessage(tv);
        tv.data = t1;
        kk.onMessage(tv);  // one press
        CHECK(rec.n == before + 2 && rec.e[before].keycode == aa::AAK_TEL && rec.e[before].down == 1 &&
              rec.e[before + 1].down == 0 && rec.apps == 4 && !strcmp(rec.appKey, "WHEEL_PHONE"));
        tv.data = t0;
        kk.onMessage(tv);
        // Voice button 15.02.512 (same status byte): stays with iDrive, nothing reaches Android Auto.
        evo::VmostMsgView pt = tv;
        pt.fkt = 0x512;
        {
            const int b0 = rec.n;
            uint8_t p1[1] = {1}, p0[1] = {0};
            pt.data = p1;
            kk.onMessage(pt);
            pt.data = p0;
            kk.onMessage(pt);
            CHECK(rec.n == b0);
        }
        const int beforeHidden = rec.n;
        kk.setOwned(false);
        tv.data = t1;
        kk.onMessage(tv);
        CHECK(rec.n == beforeHidden);  // AA not on screen: iDrive's
    }
    // Volume level, AudioMaster 20.00.C08 Status [PosX, PosY, value, min, max], and the Get that opens the filter.
    {
        evo::VmostMsgView lv;
        evo::VolumeLevel level = {0, 0, 0};
        uint8_t ok[5] = {0x01, 0x00, 17, 0, 40}, top[5] = {1, 0, 40, 0, 40}, over[5] = {1, 0, 41, 0, 40},
                flat[5] = {1, 0, 0, 0, 0};
        lv.addr = 0x0100;
        lv.fblock = 0x20;
        lv.inst = 0x00;
        lv.fkt = 0xC08;
        lv.op = 0xC;
        lv.len = 5;
        lv.data = ok;
        CHECK(evo::decodeVolumeLevel(lv, level) && level.value == 17 && level.min == 0 && level.max == 40);
        lv.data = top;
        CHECK(evo::decodeVolumeLevel(lv, level) && level.value == 40);
        lv.data = over;
        CHECK(!evo::decodeVolumeLevel(lv, level));  // above max
        lv.data = flat;
        CHECK(!evo::decodeVolumeLevel(lv, level));  // no range
        lv.data = ok;
        lv.len = 4;
        CHECK(!evo::decodeVolumeLevel(lv, level));  // cut short
        lv.len = 5;
        lv.op = 0xF;
        CHECK(!evo::decodeVolumeLevel(lv, level));  // error answer, e.g. to our Get
        lv.op = 0xC;
        lv.fkt = 0xC09;
        CHECK(!evo::decodeVolumeLevel(lv, level));
        lv.fkt = 0xC08;
        // The whole array (PosX 0): its first record, the front display's. Another display's record is not ours.
        uint8_t whole[8] = {0x00, 0x00, 21, 0, 40, 9, 0, 40}, second[5] = {0x02, 0x00, 9, 0, 40},
                column[5] = {0x01, 0x01, 9, 0, 40};
        lv.data = whole;
        lv.len = 8;
        CHECK(evo::decodeVolumeLevel(lv, level) && level.value == 21);
        lv.len = 5;
        lv.data = second;
        CHECK(!evo::decodeVolumeLevel(lv, level));
        lv.data = column;
        CHECK(!evo::decodeVolumeLevel(lv, level));
        lv.data = ok;
        static const uint8_t record[2] = {0x01, 0x00};
        evo::VmostMsgOut lg;
        evo::buildVmostGet(lg, 0x20, 0x00, 0xC08, record, 2);
        CHECK(lg.op == 1 && lg.len == 2 && lg.data == record && lg.fblock == 0x20 && lg.fkt == 0xC08);
        // Through the knob: the callback gets the level, whoever owns the screen.
        struct LevelSink {
            unsigned n, last;
            static void cb(const evo::VolumeLevel& l, void* u) {
                LevelSink* s = static_cast<LevelSink*>(u);
                ++s->n;
                s->last = l.value;
            }
        } sink = {0, 0};
        evo::EvoKnob lk;
        lk.setVolumeLevelCallback(LevelSink::cb, &sink);
        lk.onMessage(lv);
        CHECK(sink.n == 1 && sink.last == 17);
        lv.op = 0xF;
        lk.onMessage(lv);
        CHECK(sink.n == 1);
    }
    // Volume buttons (HmiMain3D CDevVolumeControl): CECenterStack 15.0C.C01 [AudioVolume, Steps],
    // ControlElementsMFL 15.02.401 [UWord, AudioVolume, Steps]; AudioVolume 1/2 = a step, 0/3 = nothing.
    {
        evo::VmostMsgView vv;
        unsigned st = 99;
        uint8_t cs1[2] = {1, 2}, cs2[2] = {2, 1}, cs0[2] = {0, 0}, cs3[2] = {3, 1}, mfl[4] = {0x00, 0x10, 2, 1};
        vv.addr = 0x0100;
        vv.fblock = 0x15;
        vv.inst = 0x0C;
        vv.fkt = 0xC01;
        vv.op = 0xC;
        vv.len = 2;
        vv.data = cs1;
        CHECK(evo::decodeVolumeButton(vv, &st) == 1 && st == 2);
        vv.data = cs2;
        CHECK(evo::decodeVolumeButton(vv, &st) == 1 && st == 1);
        vv.data = cs0;
        CHECK(evo::decodeVolumeButton(vv, &st) == 0);
        vv.data = cs3;
        CHECK(evo::decodeVolumeButton(vv, &st) == 0);
        vv.data = cs1;
        vv.op = 0xF;  // error, not status
        CHECK(evo::decodeVolumeButton(vv, &st) == 0);
        vv.op = 0xC;
        vv.fblock = 0xE5;
        CHECK(evo::decodeVolumeButton(vv, &st) == 0);
        vv.fblock = 0x15;
        vv.inst = 0x02;
        vv.fkt = 0x401;
        vv.len = 4;
        vv.data = mfl;
        CHECK(evo::decodeVolumeButton(vv, &st) == 1 && st == 1);
        vv.len = 2;
        CHECK(evo::decodeVolumeButton(vv, &st) == 0);  // short MFL frame
        // The audio knob's push button, CECenterStack 15.0C.C04 [EPushButton]: 1 pressed, 0 released.
        {
            uint8_t pressed[1] = {1}, released[1] = {0};
            evo::VmostMsgView ab = vv;
            ab.inst = 0x0C;
            ab.fkt = 0xC04;
            ab.len = 1;
            ab.data = pressed;
            CHECK(evo::decodeAudioButton(ab) == 1 && evo::decodeVolumeButton(ab, &st) == 0);
            ab.data = released;
            CHECK(evo::decodeAudioButton(ab) == 0);
            ab.data = pressed;
            ab.inst = 0x02;
            CHECK(evo::decodeAudioButton(ab) == 0);  // the steering wheel's instance
            ab.inst = 0x0C;
            ab.op = 0xF;
            CHECK(evo::decodeAudioButton(ab) == 0);
            ab.op = 0xC;
            struct Presses {
                static void cb(void* u) { ++*static_cast<unsigned*>(u); }
            };
            unsigned presses = 0;
            evo::EvoKnob kb;
            kb.setAudioButtonCallback(Presses::cb, &presses);
            kb.onMessage(ab);
            ab.data = released;
            kb.onMessage(ab);
            CHECK(presses == 1);
        }
        struct Count {
            static void cb(unsigned s, void* u) { *static_cast<unsigned*>(u) += s; }
        };
        unsigned total = 0;
        evo::EvoKnob kv;
        kv.setVolumeCallback(&Count::cb, &total);
        vv.len = 4;
        kv.onMessage(vv);
        vv.inst = 0x0C;
        vv.fkt = 0xC01;
        vv.len = 2;
        vv.data = cs1;
        kv.onMessage(vv);
        vv.data = cs0;
        kv.onMessage(vv);
        CHECK(total == 3);  // 1 (wheel) + 2 (knob), the idle frame ignored
    }
    {  // Older controller protocol (instance 0x01, other BMW models): knob, centre push, tilt, hard key.
        aa::CcsDecoder d;
        aa::CcsEvent ev[8];
        const uint8_t base[7] = {0x01, 0x1D, 0x21, 0x00, 0x10, 0x1E, 0x00};
        assert(d.feed(0xE5, 0x01, 0xC10, 0xC, base, 7, ev, 8) == 0);  // the first position only sets the base
        const uint8_t detent[7] = {0x01, 0x1D, 0x22, 0x00, 0x11, 0x1E, 0x00};
        assert(d.feed(0xE5, 0x01, 0xC10, 0xC, detent, 7, ev, 8) == 1 && ev[0].kind == 2 && ev[0].delta == 1);
        const uint8_t notConfigured[7] = {0x01, 0x1D, 0xFF, 0x00, 0x12, 0x1E, 0x00};
        assert(d.feed(0xE5, 0x01, 0xC10, 0xC, notConfigured, 7, ev, 8) == 0);
        const uint8_t push[7] = {0x01, 0x1D, 0x23, 0x01, 0x00, 0x1E, 0x01};
        assert(d.feed(0xE5, 0x01, 0xC20, 0xC, push, 7, ev, 8) == 1 && ev[0].keycode == aa::AAK_DPAD_CENTER &&
               ev[0].down);
        const uint8_t tilt[7] = {0x01, 0x1D, 0x24, 0x01, 0x01, 0x1D, 0x00};
        assert(d.feed(0xE5, 0x01, 0xC20, 0xC, tilt, 7, ev, 8) == 1 && ev[0].keycode == aa::AAK_DPAD_UP && ev[0].down);
        const uint8_t menu[7] = {0x01, 0x1D, 0x25, 0x01, 0x00, 0x00, 0x01};
        assert(d.feed(0xE5, 0x01, 0xC20, 0xC, menu, 7, ev, 8) == 1 && ev[0].keycode == aa::AAK_HOME && ev[0].down);
        assert(d.feed(0xE5, 0x03, 0xC10, 0xC, detent, 7, ev, 8) == 0);  // not on the CCS35up instance
    }
    {  // Which protocol is read: the older one only on a car whose controller is silent on CCS35up.
        using aa::CcsProtocol;
        CcsProtocol never;
        CHECK(!never.olderDue(60000) && never.kind() == CcsProtocol::kUnknown);  // nothing was read yet
        CcsProtocol g20;  // answers its reads at once: never asked for the older functions
        g20.ccs35upAsked(1000);
        CHECK(g20.accept(0x03, 0xC46) && g20.kind() == CcsProtocol::kCcs35up);
        CHECK(!g20.olderDue(1000 + CcsProtocol::kSilenceMs) && !g20.olderDue(600000));
        CHECK(!g20.accept(0x01, 0xC10) && !g20.accept(0x01, 0xC20) && g20.accept(0x03, 0xC4F));
        CHECK(g20.accept(0x01, 0xC30));  // a function of neither protocol is not judged
        CcsProtocol i3;                  // silent on CCS35up: asked once, then kept on the older protocol
        i3.ccs35upAsked(0xFFFFFF00u);    // the clock wraps while waiting
        CHECK(i3.accept(0x01, 0xC30) && i3.kind() == CcsProtocol::kUnknown);
        CHECK(!i3.olderDue(0xFFFFFF00u + CcsProtocol::kSilenceMs - 1));
        CHECK(i3.olderDue(0xFFFFFF00u + CcsProtocol::kSilenceMs) && !i3.olderDue(5000));
        CHECK(i3.accept(0x01, 0xC10) && i3.kind() == CcsProtocol::kOlder && !i3.accept(0x03, 0xC46));
        CHECK(CcsProtocol::of(0x03, 0xC10) == CcsProtocol::kUnknown &&
              CcsProtocol::of(0x01, 0xC46) == CcsProtocol::kUnknown);

        // Through EvoKnob: the older controller's knob and BACK reach Android Auto, a later CCS35up message does not.
        struct Got {
            static void ev(const aa::CcsEvent& e, void* u) {
                Got* g = static_cast<Got*>(u);
                if (g->n < 8) {
                    g->e[g->n++] = e;
                }
            }
            aa::CcsEvent e[8];
            int n;
        } got;
        memset(&got, 0, sizeof(got));
        evo::EvoKnob old;
        old.attachForTest(evo::INPUT_FORWARD, &Got::ev, &got);
        old.setOwned(true);
        evo::VmostMsgView ov;
        ov.addr = 0x0100;
        ov.fblock = 0xE5;
        ov.inst = 0x01;
        ov.op = 0xC;
        const uint8_t other[4] = {0x01, 0x1D, 0x06, 0x00};  // the i3's only E5 message before the fix
        ov.fkt = 0xC30;
        ov.len = 4;
        ov.data = other;
        old.onMessage(ov);
        CHECK(got.n == 0 && old.protocol() == CcsProtocol::kUnknown);
        const uint8_t base[7] = {0x01, 0x1D, 0x21, 0x00, 0x10, 0x1E, 0x00};
        const uint8_t detent[7] = {0x01, 0x1D, 0x22, 0x00, 0x11, 0x1E, 0x00};
        ov.fkt = 0xC10;
        ov.len = 7;
        ov.data = base;
        old.onMessage(ov);
        ov.data = detent;
        old.onMessage(ov);
        CHECK(old.protocol() == CcsProtocol::kOlder && got.n == 1 && got.e[0].kind == 2 && got.e[0].delta == 1);
        const uint8_t back[7] = {0x01, 0x1D, 0x23, 0x01, 0x00, 0x00, 0x02};
        ov.fkt = 0xC20;
        ov.data = back;
        old.onMessage(ov);
        CHECK(got.n == 2 && got.e[1].keycode == aa::AAK_BACK && got.e[1].down == 1);
        const uint8_t turned[8] = {0x0C, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
        const uint8_t turnedOn[8] = {0x0D, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0xFF};
        ov.inst = 0x03;
        ov.fkt = 0xC46;
        ov.len = 8;
        ov.data = turned;
        old.onMessage(ov);
        ov.data = turnedOn;
        old.onMessage(ov);
        CHECK(got.n == 2);  // the other protocol is left out: one action, one event
    }
    {  // Which key each button is, from the car's coding, as iDrive reads it (controller_layout.hpp).
        using aa::ControllerCoding;
        const ControllerCoding g = aa::defaultControllerCoding();  // no coding file: the table used before
        CHECK(!g.known && g.gStyle);
        const unsigned bits[7] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40};
        for (int i = 0; i < 7; ++i) {
            CHECK(aa::olderSensorSlot(bits[i], g) == i && aa::ccs35upByteSlot(i, g) == i);  // G: bit n = byte n
        }
        ControllerCoding f = g;  // F20 ZBE3 (#55): no G-style variant
        f.known = true;
        f.gStyle = false;
        CHECK(aa::olderSensorCode(0x08, f) == 5 && aa::olderSensorSlot(0x08, f) == aa::kSlotMedia);  // its media key
        CHECK(aa::olderSensorCode(0x40, f) == 6 && aa::olderSensorSlot(0x40, f) == aa::kSlotCom);
        CHECK(aa::olderSensorCode(0x20, f) == 17 && aa::olderSensorSlot(0x20, f) == aa::kSlotNav);  // F's NAV key
        CHECK(aa::olderSensorSlot(0x01, f) == aa::kSlotMenu && aa::olderSensorSlot(0x02, f) == aa::kSlotBack &&
              aa::olderSensorSlot(0x04, f) == aa::kSlotOptions && aa::olderSensorSlot(0x10, f) == aa::kSlotMedia);
        CHECK(aa::ccs35upByteSlot(3, f) == aa::kSlotMedia && aa::ccs35upByteSlot(5, f) == aa::kSlotNav &&
              aa::ccs35upByteSlot(6, f) == aa::kSlotCom);
        ControllerCoding f5141 = f;  // the F20 of #55 as it reported it (b80=0 5141=1 4e59=1 4f1e=0 5486=0 role=1):
        f5141.v5141 = true;          // its MEDIA (0x08) is iDrive's code 15, MEDIA for Android Auto; COM still COM
        f5141.v4f1e = f5141.v5486 = false;
        f5141.role = 1;
        CHECK(aa::olderSensorCode(0x08, f5141) == 15 && aa::olderSensorSlot(0x08, f5141) == aa::kSlotMedia);
        CHECK(aa::olderSensorSlot(0x40, f5141) == aa::kSlotCom && aa::olderSensorSlot(0x20, f5141) == aa::kNoSlot);
        ControllerCoding fNoTel = f;  // without 0x4E59, 0x40 is nothing on an F controller
        fNoTel.v4e59 = false;
        CHECK(aa::olderSensorSlot(0x40, fNoTel) == aa::kNoSlot);
        ControllerCoding gRole1 = g;  // G, role 1, without 0x4E59: 0x08 is iDrive's RADIO (5), MEDIA
        gRole1.v4e59 = false;
        gRole1.role = 1;
        CHECK(aa::olderSensorCode(0x08, gRole1) == 5 && aa::olderSensorSlot(0x08, gRole1) == aa::kSlotMedia);
        ControllerCoding noNav = g;  // neither 0x4F1E nor 0x5486: 0x20 is no key
        noNav.v4f1e = noNav.v5486 = false;
        CHECK(aa::olderSensorSlot(0x20, noNav) == aa::kNoSlot);
        CHECK(aa::olderSensorSlot(0x03, g) == aa::kNoSlot && aa::olderSensorSlot(0x80, g) == aa::kNoSlot);

        char line[96];
        aa::formatControllerCoding(f, line, sizeof(line));
        CHECK(!strcmp(line, "coding b80=0 5141=0 4e59=1 4f1e=1 5486=1 role=0"));
        ControllerCoding back = g;
        CHECK(aa::parseControllerCoding(line, back) && back.known && !back.gStyle && back.v4e59 && back.role == 0);
        CHECK(aa::parseControllerCoding("coding b80=1 5141=1 4e59=0 4f1e=0 5486=1 role=2\n", back) && back.gStyle &&
              back.v5141 && !back.v4e59 && !back.v4f1e && back.v5486 && back.role == 2);
        ControllerCoding kept = g;
        CHECK(!aa::parseControllerCoding("coding b80=2 5141=0 4e59=1 4f1e=1 5486=1 role=0", kept) && !kept.known);
        CHECK(!aa::parseControllerCoding("coding b80=1", kept) && !aa::parseControllerCoding(nullptr, kept));

        // The F20 own messages (#55), with its reported coding: MEDIA (0x08) is MEDIA, COM (0x40) is COM.
        aa::CcsDecoder d;
        evo::configureHardKeys(d);
        d.setCoding(f5141);  // the coding the F20 reported
        aa::CcsEvent ev[8];
        const uint8_t media[7] = {0x01, 0x1D, 0x3F, 0x01, 0x00, 0x00, 0x08};
        CHECK(d.feed(0xE5, 0x01, 0xC20, 0xC, media, 7, ev, 8) == 1 && ev[0].keycode == aa::AAK_MEDIA && ev[0].down &&
              !strcmp(ev[0].what, "MEDIA"));
        const uint8_t mediaUp[7] = {0x01, 0x1D, 0x40, 0x00, 0x00, 0x00, 0x08};
        CHECK(d.feed(0xE5, 0x01, 0xC20, 0xC, mediaUp, 7, ev, 8) == 1 && ev[0].keycode == aa::AAK_MEDIA && !ev[0].down);
        const uint8_t com[7] = {0x01, 0x1D, 0xF1, 0x01, 0x00, 0x00, 0x40};
        CHECK(d.feed(0xE5, 0x01, 0xC20, 0xC, com, 7, ev, 8) == 1 && ev[0].keycode == aa::AAK_TEL &&
              !strcmp(ev[0].what, "COM"));
        CHECK(d.sensorSlot(0x08) == aa::kSlotMedia && d.sensorSlot(0x40) == aa::kSlotCom);
        // A CCS35up on an F coding: byte 3 (iDrive's RADIO) is MEDIA, byte 6 COM, each with its own state.
        aa::CcsDecoder c;
        evo::configureHardKeys(c);
        c.setCoding(f);
        const uint8_t radio[7] = {0, 0, 0, 1, 0, 0, 0}, radioAndMedia[7] = {0, 0, 0, 1, 1, 0, 0}, none[7] = {0};
        CHECK(c.feed(0xE5, 0x03, 0xC47, 0xC, radio, 7, ev, 8) == 1 && ev[0].keycode == aa::AAK_MEDIA && ev[0].down);
        CHECK(c.feed(0xE5, 0x03, 0xC47, 0xC, radioAndMedia, 7, ev, 8) == 1 && ev[0].keycode == aa::AAK_MEDIA);
        CHECK(c.feed(0xE5, 0x03, 0xC47, 0xC, none, 7, ev, 8) == 2 && !ev[0].down && !ev[1].down);

        // Double MEDIA through EvoKnob on the F20: its media key (0x08) is seen, COM (0x40) cancels the gesture.
        struct Media {
            static void cb(unsigned state, void* u) {
                Media* m = static_cast<Media*>(u);
                if (m->n < 8) {
                    m->s[m->n++] = state;
                }
            }
            static void key(const aa::CcsEvent&, void*) {}
            unsigned s[8];
            int n;
        } seen;
        memset(&seen, 0, sizeof(seen));
        evo::EvoKnob f20;
        f20.attachForTest(evo::INPUT_FORWARD, &Media::key, &seen);
        f20.setCoding(f5141);
        f20.setMediaCallback(&Media::cb, &seen);
        evo::VmostMsgView mv;
        mv.addr = 0x0100;
        mv.fblock = 0xE5;
        mv.inst = 0x01;
        mv.fkt = 0xC20;
        mv.op = 0xC;
        mv.len = 7;
        mv.data = media;
        f20.onMessage(mv);
        mv.data = mediaUp;
        f20.onMessage(mv);
        // An edge-only protocol: released before the press, then pressed, then released.
        CHECK(seen.n == 3 && seen.s[0] == 0 && seen.s[1] == 1 && seen.s[2] == 0);
        const int before = seen.n;
        mv.data = com;
        f20.onMessage(mv);
        CHECK(seen.n > before && seen.s[seen.n - 1] == evo::MediaDoublePress::kOtherInput);
    }
    puts(
        "PASS ancienne molette BMW (C10/C20), lue seulement si la CCS35up reste muette, un seul protocole par voiture "
        "; "
        "entrees : IPv4/UDP de la copie, machine tactile (appui, glisse, "
        "relache, doublon, trame fragmentee, relache forcee, rien avant l'image, relache d'un doigt vu par le "
        "telephone apres le depart de l'image), message vMOST -> decodeur molette ; "
        "touches comme CarPlay (effleurement ignore, NAV/MEDIA/COM et bouton telephone du volant -> applis AA, COM a "
        "l iDrive pendant un appel, sauf le relache d'un COM appuye avant) ; touches selon le codage de la voiture, "
        "comme l iDrive (controleur G ou F, F20 ZBE3 #55, double MEDIA) ; boutons de volume console/volant decodes ; "
        "niveau du volume (20.00.C08)");
    return 0;
}
