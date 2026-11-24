// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Double MEDIA through the real controller decoder, guard transitions and one-shot requests in temporary files.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <string>

#include "evo_knob.hpp"
#include "media_recall.hpp"

#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)

namespace {
bool click(evo::MediaDoublePress& d, uint32_t at, uint32_t duration = 40, bool allowed = true) {
    CHECK(!d.feed(1, at, allowed));
    return d.feed(0, at + duration, allowed);
}

void gestures() {
    evo::MediaDoublePress d;
    CHECK(!d.feed(0, 0, true));
    CHECK(!click(d, 100));         // one ordinary MEDIA click does not recall
    CHECK(!d.feed(0, 145, true));  // duplicate release is not another click
    CHECK(!d.feed(1, 200, true));
    CHECK(!d.feed(1, 210, true));  // C47 / C4F carry the same held state
    CHECK(d.feed(4, 240, true));   // released, finger still resting on the key
    CHECK(!d.feed(4, 250, true));
    CHECK(!click(d, 300));  // a triple click makes one recall
    d.cancel();
    CHECK(!click(d, 1000));
    CHECK(!click(d, 1501));  // releases more than 500 ms apart: two singles
    CHECK(click(d, 1600));
    CHECK(!click(d, 2000, 501));  // a long physical hold may still report state 1
    CHECK(!click(d, 2600));
    CHECK(!d.feed(2, 2660, true));  // explicit hold cancels the first click
    CHECK(!d.feed(0, 2670, true));
    CHECK(!click(d, 2700));
    CHECK(!d.feed(7, 2760, true));  // unknown status cannot join two gestures
    CHECK(!d.feed(0, 2770, true));
    CHECK(!click(d, 2800));
    CHECK(!d.feed(3, 2860, true));
    CHECK(!d.feed(0, 2870, true));
    CHECK(!click(d, 2900));
    d.cancel();
    CHECK(!click(d, 3000, 40, false));  // first press during call/foreground/Stop is not remembered
    CHECK(!click(d, 3100));
    d.cancel();  // even if a guard toggled without a controller message in between
    CHECK(!click(d, 3200));
    CHECK(click(d, 3300));

    evo::MediaDoublePress held;
    CHECK(!held.feed(1, 0, true));  // initial Get while already held is not a press edge
    CHECK(!held.feed(0, 10, true));
    CHECK(!click(held, 20));
    CHECK(click(held, 100));

    evo::MediaDoublePress wrap;
    CHECK(!wrap.feed(0, 0xFFFFFF00u, true));
    CHECK(!click(wrap, 0xFFFFFFF0u));
    CHECK(click(wrap, 100));  // the head unit's 32-bit millisecond clock wraps

    evo::MediaDoublePress boundary;
    CHECK(!boundary.feed(0, 0, true));
    CHECK(!click(boundary, 100, 500));
    CHECK(click(boundary, 1060));  // releases exactly 500 ms apart
}

void guardsAndFiles() {
    for (unsigned bits = 0; bits < 32; ++bits) {
        CHECK(evo::mediaRecallAllowed(bits & 1, bits & 2, bits & 4, bits & 8, bits & 16) == (bits == 1));
    }
    char folder[] = "/tmp/media-recallXXXXXX";
    CHECK(mkdtemp(folder));
    const std::string path = std::string(folder) + "/request";
    const int pid = static_cast<int>(getpid());
    CHECK(!evo::takeMediaRecall(path.c_str(), pid, 100));
    CHECK(evo::writeMediaRecall(path.c_str(), pid, 100));
    CHECK(evo::takeMediaRecall(path.c_str(), pid, 2100));
    CHECK(!evo::takeMediaRecall(path.c_str(), pid, 2100));  // one shot
    CHECK(evo::writeMediaRecall(path.c_str(), pid, 100));
    CHECK(!evo::takeMediaRecall(path.c_str(), pid, 2101));  // expired
    CHECK(evo::writeMediaRecall(path.c_str(), pid, 100));
    CHECK(!evo::takeMediaRecall(path.c_str(), pid + 1, 200));
    CHECK(!evo::takeMediaRecall(path.c_str(), pid, 200));  // old process request was consumed
    CHECK(evo::writeMediaRecall(path.c_str(), pid, 100));
    CHECK(!evo::takeMediaRecall(path.c_str(), pid, 99));  // future timestamp
    CHECK(evo::writeMediaRecall(path.c_str(), pid, 0xFFFFFFF0u));
    CHECK(evo::takeMediaRecall(path.c_str(), pid, 100));
    const char* malformed[] = {
        "", "media-recall", "media-recall 0 100", "media-recall -1 100", "other 2 100", "media-recall 2 100 extra"};
    for (unsigned i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        FILE* file = fopen(path.c_str(), "w");
        CHECK(file);
        fputs(malformed[i], file);
        fclose(file);
        CHECK(!evo::takeMediaRecall(path.c_str(), pid, 200));
        CHECK(access(path.c_str(), F_OK) != 0);
    }
    CHECK(evo::writeMediaRecall(path.c_str(), pid, 100));
    CHECK(evo::writeMediaRecall(path.c_str(), pid, 200));
    CHECK(evo::takeMediaRecall(path.c_str(), pid, 2200));  // latest complete request
    CHECK(evo::writeMediaRecall(path.c_str(), pid, 100));
    CHECK(evo::takeMediaRecall(path.c_str(), pid, 200));
    CHECK(!evo::mediaRecallAllowed(true, false, false, true, false));  // HMI rejects it during a call
    CHECK(!evo::takeMediaRecall(path.c_str(), pid, 300));              // ending that call cannot replay it
    CHECK(!evo::writeMediaRecall((path + "/missing").c_str(), pid, 100));
    CHECK(access((path + ".tmp").c_str(), F_OK) != 0);
    CHECK(access((path + ".read").c_str(), F_OK) != 0);
    CHECK(rmdir(folder) == 0);
}

struct Controller {
    evo::MediaDoublePress detector;
    uint32_t now;
    bool allowed;
    int recalls, phoneKeys, appKeys;
    Controller() : now(0), allowed(true), recalls(0), phoneKeys(0), appKeys(0) {}
    static void media(unsigned state, void* user) {
        Controller& self = *static_cast<Controller*>(user);
        if (self.detector.feed(state, self.now, self.allowed)) {
            ++self.recalls;
        }
    }
    static void phone(const aa::CcsEvent&, void* user) { ++static_cast<Controller*>(user)->phoneKeys; }
    static void app(const char*, void* user) { ++static_cast<Controller*>(user)->appKeys; }
};

void controller(bool older) {
    Controller c;
    evo::EvoKnob knob;
    knob.attachForTest(evo::INPUT_FORWARD, &Controller::phone, &c);
    knob.setMediaCallback(&Controller::media, &c);
    knob.setAppKeyCallback(&Controller::app, &c);
    uint8_t bytes[8] = {};
    evo::VmostMsgView message = {
        0x0100, 0xE5, static_cast<uint8_t>(older ? 1 : 3), static_cast<uint16_t>(older ? 0xC20 : 0xC47), 0xC, 7, bytes};
    if (older) {
        bytes[2] = 1;
        bytes[6] = 1u << 4;
    }
    const unsigned slot = older ? 3 : 4;
    knob.onMessage(message);
    for (unsigned i = 0; i < 4; ++i) {
        c.now += 80;
        bytes[slot] = (i % 2) == 0 ? 1 : 0;
        knob.onMessage(message);
        if (!older) {
            message.fkt = 0xC4F;
            message.len = 8;
            knob.onMessage(message);
            message.fkt = 0xC47;
            message.len = 7;
        }
        CHECK(c.recalls == (i == 3 ? 1 : 0));
    }
    CHECK(c.phoneKeys == 0 && c.appKeys == 0);  // hidden: every original event remains iDrive's
    c.allowed = false;
    knob.setOwned(true);
    for (unsigned i = 0; i < 4; ++i) {
        c.now += 80;
        bytes[slot] = (i % 2) == 0 ? 1 : 0;
        knob.onMessage(message);
    }
    CHECK(c.recalls == 1 && c.phoneKeys == 4 && c.appKeys == 2);  // existing foreground MEDIA behavior
    c.allowed = true;
    knob.setOwned(false);
    c.now += 80;
    bytes[slot] = 1;
    knob.onMessage(message);
    c.now += 40;
    bytes[slot] = 0;
    knob.onMessage(message);
    CHECK(c.recalls == 1);
    // Another key between MEDIA clicks cancels the pair, even though both would fit in the timing window.
    if (older) {
        bytes[6] = 1u << 5;
        bytes[slot] = 1;
    } else {
        bytes[5] = 1;
    }
    knob.onMessage(message);
    if (older) {
        bytes[6] = 1u << 4;
    } else {
        bytes[5] = 0;
    }
    bytes[slot] = 0;
    knob.onMessage(message);
    c.now += 80;
    bytes[slot] = 1;
    knob.onMessage(message);
    c.now += 40;
    bytes[slot] = 0;
    knob.onMessage(message);
    CHECK(c.recalls == 1);
    // Observe-only mode must not create requests.
    knob.attachForTest(evo::INPUT_OBSERVE, &Controller::phone, &c);
    for (unsigned i = 0; i < 4; ++i) {
        c.now += 80;
        bytes[slot] = (i % 2) == 0 ? 1 : 0;
        knob.onMessage(message);
    }
    CHECK(c.recalls == 1);
}
// The older controller reports edges only, one message for the key concerned. As on the F20 of #55: MENU, then a
// quick double MEDIA brings Android Auto back, right after the service started and again later. Before: the key
// pressed in between made the detector forget MEDIA's state, and the next MEDIA press was lost.
void afterAnotherKey() {
    Controller c;
    evo::EvoKnob knob;
    knob.attachForTest(evo::INPUT_FORWARD, &Controller::phone, &c);
    knob.setMediaCallback(&Controller::media, &c);
    uint8_t bytes[8] = {0x01, 0x1D, 0x10, 0, 0, 0, 0, 0};
    evo::VmostMsgView message = {0x0100, 0xE5, 1, 0xC20, 0xC, 7, bytes};
    struct Edge {
        static void send(evo::EvoKnob& k, evo::VmostMsgView& m, uint8_t* b, uint8_t sensor, bool down) {
            ++b[2];
            b[3] = down ? 1 : 0;
            b[6] = sensor;
            k.onMessage(m);
        }
    };
    const uint8_t menu = 0x01, media = 0x10;       // G-style table (no coding file)
    Edge::send(knob, message, bytes, menu, true);  // the first key since the service started
    c.now += 120;
    Edge::send(knob, message, bytes, menu, false);
    for (int round = 0; round < 2; ++round) {
        c.now += 2000;
        Edge::send(knob, message, bytes, media, true);
        c.now += 61;
        Edge::send(knob, message, bytes, media, false);
        c.now += 81;
        Edge::send(knob, message, bytes, media, true);
        c.now += 81;
        Edge::send(knob, message, bytes, media, false);
        CHECK(c.recalls == round + 1);
        c.now += 2000;  // MENU again before the next round
        Edge::send(knob, message, bytes, menu, true);
        c.now += 120;
        Edge::send(knob, message, bytes, menu, false);
    }
    CHECK(c.phoneKeys == 0);  // Android Auto hidden: nothing sent to the phone
}

}  // namespace

int main() {
    gestures();
    guardsAndFiles();
    controller(false);
    controller(true);
    afterAnotherKey();
    puts(
        "PASS double MEDIA: short clicks, duplicate states, guards, session-bound requests, both controllers, "
        "foreground keys");
    return 0;
}
