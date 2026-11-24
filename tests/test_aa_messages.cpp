// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The head unit's messages byte for byte, the two readers, and which handler takes each message (core/aa_messages,
// core/aa_protocol.hpp), on the PC.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "aa_messages.hpp"
#include "aa_protocol.hpp"

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                            \
        }                                                       \
    } while (0)

namespace {

typedef std::vector<uint8_t> Bytes;

void messages() {
    using namespace aa::message;
    CHECK(versionRequest() == Bytes({0x00, 0x01, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00}));
    CHECK(tlsHandshake(Bytes({0x16, 0x03})) == Bytes({0x00, 0x03, 0x16, 0x03}));
    CHECK(authComplete() == Bytes({0x00, 0x04, 0x08, 0x00}));
    CHECK(discoveryResponse(Bytes({0x0a})) == Bytes({0x00, 0x06, 0x0a}));
    CHECK(channelOpenResponse() == Bytes({0x00, 0x08, 0x08, 0x00}));
    CHECK(audioFocusResponse(3) == Bytes({0x00, 0x13, 0x08, 0x03}));
    CHECK(navigationFocusResponse() == Bytes({0x00, 0x0e, 0x08, 0x02}));
    CHECK(byeByeResponse() == Bytes({0x00, 0x10}));

    // The ping's timestamp comes back as it was written, cut at its last byte or at 16 bytes of answer.
    CHECK(pingResponse(Bytes({0x00, 0x0b, 0x08, 0x96, 0x01, 0x10, 0x00})) == Bytes({0x00, 0x0c, 0x08, 0x96, 0x01}));
    CHECK(pingResponse(Bytes({0x00, 0x0b, 0x08, 0x80, 0x00})) == Bytes({0x00, 0x0c, 0x08, 0x80, 0x00}));
    CHECK(pingResponse(Bytes({0x00, 0x0b, 0x10, 0x01})) == Bytes({0x00, 0x0c}));
    CHECK(pingResponse(Bytes({0x00, 0x0b, 0x08})) == Bytes({0x00, 0x0c}));
    Bytes endless({0x00, 0x0b, 0x08});
    endless.insert(endless.end(), 20, 0xff);
    CHECK(pingResponse(endless).size() == 16);
    // Our own ping: the timestamp alone, as field 1; the phone's answer to it echoes it back.
    CHECK(pingRequest(150) == Bytes({0x00, 0x0b, 0x08, 0x96, 0x01}));
    CHECK(pingRequest(0) == Bytes({0x00, 0x0b, 0x08, 0x00}));
    Bytes ours = pingRequest(1700000000123456ull);  // a clock in microseconds: an 8-byte varint
    CHECK(ours.size() == 11);
    ours[1] = 0x0c;
    CHECK(pingResponse(pingRequest(1700000000123456ull)) == ours);

    CHECK(setupResponse() == Bytes({0x80, 0x03, 0x08, 0x02, 0x10, 0x0a, 0x18, 0x00}));
    CHECK(videoFocusIndication(true, false) == Bytes({0x80, 0x08, 0x08, 0x01, 0x10, 0x00}));
    CHECK(videoFocusIndication(false, true) == Bytes({0x80, 0x08, 0x08, 0x02, 0x10, 0x01}));
    CHECK(mediaAck(289) == Bytes({0x80, 0x04, 0x08, 0xa1, 0x02, 0x10, 0x01}));
    CHECK(micOpenResponse() == Bytes({0x80, 0x06, 0x08, 0x00, 0x10, 0x00}));
    CHECK(micData(0x0102030405060708ull, Bytes({0xaa})) ==
          Bytes({0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xaa}));
    CHECK(sensorStartResponse(true) == Bytes({0x80, 0x02, 0x08, 0x00}));
    CHECK(nightModeEvent(true) == Bytes({0x80, 0x03, 0x52, 0x02, 0x08, 0x01}));
    CHECK(drivingStatusEvent(0) == Bytes({0x80, 0x03, 0x6a, 0x02, 0x08, 0x00}));
    CHECK(bindingResponse() == Bytes({0x80, 0x03, 0x08, 0x00}));

    CHECK(keyEvent(5, 3, true) ==
          Bytes({0x80, 0x01, 0x08, 0x05, 0x22, 0x0a, 0x0a, 0x08, 0x08, 0x03, 0x10, 0x01, 0x18, 0x00, 0x20, 0x00}));
    // A negative step is a ten-byte varint, as protobuf writes an int32.
    CHECK(rotaryEvent(5, 65536, -1) == Bytes({0x80, 0x01, 0x08, 0x05, 0x32, 0x11, 0x0a, 0x0f, 0x08, 0x80, 0x80, 0x04,
                                              0x10, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}));
    aa::TouchEventOut touch;
    memset(&touch, 0, sizeof(touch));
    touch.n = 1;
    touch.p[0].x = -5;
    touch.p[0].y = 470;
    touch.p[0].id = 1;
    touch.index = 0;
    touch.action = 6;
    // A negative coordinate is a 32-bit value: five bytes.
    CHECK(touchEvent(5, touch) == Bytes({0x80, 0x01, 0x08, 0x05, 0x1a, 0x11, 0x0a, 0x0b, 0x08, 0xfb, 0xff, 0xff,
                                         0xff, 0x0f, 0x10, 0xd6, 0x03, 0x18, 0x01, 0x10, 0x00, 0x18, 0x06}));
}

void readers() {
    CHECK(aa::messageId(Bytes({0x80, 0x07, 0x08})) == 0x8007);
    CHECK(aa::messageId(Bytes({0x05})) == 0 && aa::messageId(Bytes()) == 0);
    CHECK(aa::bodyField(Bytes({0x80, 0x01, 0x08, 0x0d, 0x10, 0x05}), 2) == 5);
    CHECK(aa::bodyField(Bytes({0x80, 0x01, 0x10, 0x05}), 1) == aa::kNoField);
    CHECK(aa::bodyField(Bytes({0x80, 0x01}), 1) == aa::kNoField);
    CHECK(aa::bodyField(Bytes({0x80, 0x01, 0x08}), 1) == aa::kNoField);
    CHECK(aa::bodyField(Bytes({0x80, 0x01, 0x08, 0x8a, 0x80, 0x80, 0x80, 0x10}), 1) == 10);  // cut to 32 bits
}

void routes() {
    using aa::Route;
    using aa::routeOf;
    // Channel services first, though their ids are media ids.
    CHECK(routeOf(1, 0x8001) == Route::kSensorStart);
    CHECK(routeOf(14, 0x8005) == Route::kMicrophoneOpen && routeOf(14, 0x8004) == Route::kMicrophoneAck);
    CHECK(routeOf(10, 0x8007) == Route::kNavigationStatus && routeOf(10, 0x8000) == Route::kNavigationStatus);
    CHECK(routeOf(10, 0x8002) == Route::kNavigationStatus && routeOf(10, 0xffff) == Route::kNavigationStatus);
    CHECK(routeOf(12, 0x8001) == Route::kPlaybackStatus && routeOf(12, 0x8000) == Route::kPlaybackStatus);
    // Channel open, on any channel, the control channel included.
    CHECK(routeOf(6, 0x0007) == Route::kChannelOpen && routeOf(0, 0x0007) == Route::kChannelOpen);
    CHECK(routeOf(10, 0x0007) == Route::kChannelOpen);
    // Media ids on every other channel.
    CHECK(routeOf(6, 0x8000) == Route::kSetup && routeOf(3, 0x8000) == Route::kSetup);
    CHECK(routeOf(14, 0x8000) == Route::kSetup && routeOf(0, 0x8000) == Route::kSetup);
    CHECK(routeOf(1, 0x8000) == Route::kSetup);
    CHECK(routeOf(8, 0x8001) == Route::kStart && routeOf(14, 0x8001) == Route::kStart);
    CHECK(routeOf(8, 0x8002) == Route::kAudioStop && routeOf(9, 0x8002) == Route::kAudioStop);
    CHECK(routeOf(3, 0x8002) == Route::kInputBinding && routeOf(5, 0x8002) == Route::kInputBinding);
    CHECK(routeOf(7, 0x8002) == Route::kInputBinding);
    CHECK(routeOf(2, 0x8002) == Route::kOther && routeOf(0, 0x8002) == Route::kControl);
    CHECK(routeOf(6, 0x8007) == Route::kVideoFocusRequest && routeOf(6, 0x8008) == Route::kVideoFocusNotice);
    CHECK(routeOf(2, 0x8007) == Route::kOther && routeOf(4, 0x8008) == Route::kOther);
    CHECK(routeOf(6, 0x0000) == Route::kMedia && routeOf(6, 0x0001) == Route::kMedia);
    CHECK(routeOf(0, 0x0000) == Route::kMedia && routeOf(0, 0x0001) == Route::kMedia);
    CHECK(routeOf(10, 0x0000) == Route::kMedia && routeOf(12, 0x0001) == Route::kMedia);
    CHECK(routeOf(1, 0x0001) == Route::kMedia && routeOf(14, 0x0000) == Route::kMedia);
    // The rest of the control channel, and anything else.
    CHECK(routeOf(0, 0x0002) == Route::kControl && routeOf(0, 0x000b) == Route::kControl);
    CHECK(routeOf(0, 0x8003) == Route::kControl && routeOf(0, 0x8008) == Route::kControl);
    CHECK(routeOf(200, 0x1234) == Route::kOther && routeOf(1, 0x8002) == Route::kOther);
    CHECK(routeOf(7, 0x8001) == Route::kStart && routeOf(1, 0x8003) == Route::kOther);
}

}  // namespace

int main() {
    messages();
    readers();
    routes();
    puts("PASS messages: every head unit message byte for byte, body readers, routes in dispatch order");
    return 0;
}
