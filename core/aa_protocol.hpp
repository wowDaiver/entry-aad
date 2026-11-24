// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto wire constants: channel numbers, frame flags, message ids and the values this head unit sends.
#ifndef ENTRY_AAD_CORE_AA_PROTOCOL_HPP
#define ENTRY_AAD_CORE_AA_PROTOCOL_HPP

#include <stddef.h>
#include <stdint.h>

namespace aa {

// Channels as the service discovery numbers them (core/include/dhu_sdp.h, adapted by core/sdr_builder.cpp).
namespace channel {
const uint8_t kControl = 0;
const uint8_t kSensors = 1;
const uint8_t kInputDisplay2 = 3;  // input sources of the template's two other displays: bound, nothing sent
const uint8_t kInputDisplay1 = 5;
const uint8_t kVideo = 6;  // the main display, the only picture shown
const uint8_t kInput = 7;  // touchscreen and keys of the main display
const uint8_t kGuidanceAudio = 8;
const uint8_t kMediaAudio = 9;  // removed from the discovery: media plays over the car's Bluetooth
const uint8_t kNavigation = 10;
const uint8_t kMediaStatus = 12;
const uint8_t kMicrophone = 14;

inline bool isAudioSink(unsigned ch) {
    return ch == kGuidanceAudio || ch == kMediaAudio;
}
// The streams the phone sends the car, where it holds at most kMaxUnacked media messages without an ack.
inline bool isMediaSink(unsigned ch) {
    return ch == kVideo || isAudioSink(ch);
}
inline bool isInput(unsigned ch) {
    return ch == kInputDisplay2 || ch == kInputDisplay1 || ch == kInput;
}
}  // namespace channel

// Frame: [channel][flags][u16 length], then a u32 total size on a FIRST fragment, then the payload.
namespace frame {
const size_t kHeaderSize = 4;
const size_t kTotalSizeSize = 4;
const size_t kMaxPayload = 0xFFFF;
const size_t kMaxMessage = 8u * 1024u * 1024u;  // reassembly bound: a lost LAST must not grow a buffer for ever

// Low two bits: where the frame sits in its message.
const uint8_t kPositionMask = 0x03;
const uint8_t kMiddle = 0x00;
const uint8_t kFirst = 0x01;
const uint8_t kLast = 0x02;
const uint8_t kWhole = 0x03;
const uint8_t kChannelControl = 0x04;  // a control message (channel open) carried on a service channel
const uint8_t kEncrypted = 0x08;

const uint8_t kPlain = kWhole;                                    // 0x03
const uint8_t kSealed = kWhole | kEncrypted;                      // 0x0B
const uint8_t kSealedChannelControl = kSealed | kChannelControl;  // 0x0F
}  // namespace frame

// Message ids: the first two bytes of every message, big-endian. Each service numbers its own from 0x8000.
namespace msg {
// Control channel; the channel open pair travels on the channel being opened.
const uint16_t kVersionRequest = 0x0001;
const uint16_t kVersionResponse = 0x0002;
const uint16_t kTlsHandshake = 0x0003;
const uint16_t kAuthComplete = 0x0004;
const uint16_t kDiscoveryRequest = 0x0005;
const uint16_t kDiscoveryResponse = 0x0006;
const uint16_t kChannelOpenRequest = 0x0007;
const uint16_t kChannelOpenResponse = 0x0008;
const uint16_t kPingRequest = 0x000B;
const uint16_t kPingResponse = 0x000C;
const uint16_t kNavigationFocusRequest = 0x000D;
const uint16_t kNavigationFocusResponse = 0x000E;
const uint16_t kByeByeRequest = 0x000F;
const uint16_t kByeByeResponse = 0x0010;
const uint16_t kVoiceSessionRequest = 0x0011;
const uint16_t kAudioFocusRequest = 0x0012;
const uint16_t kAudioFocusResponse = 0x0013;

// Audio, video and microphone channels.
const uint16_t kMediaData = 0x0000;    // 8-byte timestamp, then the data
const uint16_t kMediaConfig = 0x0001;  // codec configuration (H.264 SPS/PPS)
const uint16_t kSetupRequest = 0x8000;
const uint16_t kStartIndication = 0x8001;
const uint16_t kStopIndication = 0x8002;
const uint16_t kSetupResponse = 0x8003;
const uint16_t kMediaAck = 0x8004;
const uint16_t kMicOpenRequest = 0x8005;
const uint16_t kMicOpenResponse = 0x8006;
const uint16_t kVideoFocusRequest = 0x8007;
const uint16_t kVideoFocusIndication = 0x8008;

// Sensor channel.
const uint16_t kSensorStartRequest = 0x8001;
const uint16_t kSensorStartResponse = 0x8002;
const uint16_t kSensorEvent = 0x8003;
const uint16_t kSensorError = 0x8004;

// Input channels.
const uint16_t kInputEvent = 0x8001;
const uint16_t kBindingRequest = 0x8002;
const uint16_t kBindingResponse = 0x8003;

// Navigation and media status channels: every id from here up belongs to the service.
const uint16_t kServiceFirst = 0x8000;
const uint16_t kGuidanceState = 0x8003;  // navigation: guidance active or not
const uint16_t kPlaybackState = 0x8001;  // media status: playing, paused...
}  // namespace msg

// Values the head unit puts in its messages.
const uint16_t kVersionMajor = 1;
const uint16_t kVersionMinor = 6;
const uint32_t kStatusOk = 0;
const uint32_t kMediaStatusOk = 2;  // in a setup response
const uint32_t kMaxUnacked = 10;    // media frames the phone may send ahead of our acks

namespace sensor {
const uint32_t kLocation = 1;  // a type, and the field of its data in a sensor event
const uint32_t kNightMode = 10;
const uint32_t kDrivingStatus = 13;
const uint32_t kDrivingUnrestricted = 0;
const uint32_t kDefaultPeriodMs = 1000;  // when the phone asks for location without a period
}  // namespace sensor

namespace focus {
// Video focus modes.
const uint32_t kProjected = 1;
const uint32_t kNative = 2;
// Audio focus: what the phone asks, what the head unit grants.
const uint32_t kAudioRelease = 4;
const uint32_t kAudioGain = 1;
const uint32_t kAudioLoss = 3;
// Navigation focus granted: the phone's guidance, this head unit runs none of its own.
const uint32_t kNavigationProjected = 2;
}  // namespace focus

// Video message bodies: the codec configuration follows the id; data follows the id and an 8-byte timestamp.
const size_t kMessageIdSize = 2;
const size_t kTimestampedHeader = kMessageIdSize + 8;

// Which handler takes a message (tests/test_aa_messages.cpp).
enum class Route {
    kSensorStart,
    kMicrophoneOpen,
    kMicrophoneAck,
    kChannelOpen,
    kNavigationStatus,
    kPlaybackStatus,
    kSetup,
    kStart,
    kAudioStop,
    kInputBinding,
    kVideoFocusRequest,
    kVideoFocusNotice,
    kMedia,
    kControl,
    kOther,
};

// The order is the rule. First the services that reuse the media ids on their own channel: the generic media handling
// must not take them (taking a navigation 0x8007 for a video focus request made the phone drop the session as soon as
// guidance started, and reconnect). Then the media messages, whatever the channel; then the control channel.
inline Route routeOf(uint8_t ch, uint16_t id) {
    if (ch == channel::kSensors && id == msg::kSensorStartRequest) {
        return Route::kSensorStart;
    }
    if (ch == channel::kMicrophone && (id == msg::kMicOpenRequest || id == msg::kMediaAck)) {
        return id == msg::kMicOpenRequest ? Route::kMicrophoneOpen : Route::kMicrophoneAck;
    }
    if (id == msg::kChannelOpenRequest) {
        return Route::kChannelOpen;
    }
    if (id >= msg::kServiceFirst && (ch == channel::kNavigation || ch == channel::kMediaStatus)) {
        return ch == channel::kNavigation ? Route::kNavigationStatus : Route::kPlaybackStatus;
    }
    switch (id) {
        case msg::kSetupRequest:
            return Route::kSetup;
        case msg::kStartIndication:
            return Route::kStart;
        case msg::kStopIndication:  // also the binding request of the input channels
            if (channel::isAudioSink(ch)) {
                return Route::kAudioStop;
            }
            if (channel::isInput(ch)) {
                return Route::kInputBinding;
            }
            break;
        case msg::kVideoFocusRequest:
        case msg::kVideoFocusIndication:
            if (ch == channel::kVideo) {
                return id == msg::kVideoFocusRequest ? Route::kVideoFocusRequest : Route::kVideoFocusNotice;
            }
            break;
        case msg::kMediaData:
        case msg::kMediaConfig:
            return Route::kMedia;
        default:
            break;
    }
    return ch == channel::kControl ? Route::kControl : Route::kOther;
}

}  // namespace aa

#endif  // ENTRY_AAD_CORE_AA_PROTOCOL_HPP
