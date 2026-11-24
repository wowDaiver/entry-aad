// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The session's message handlers, one per route of core/aa_protocol.hpp, grouped by channel.
#include <cstdio>

#include "aa_protocol.hpp"
#include "aa_session.hpp"
#include "crash_handler.hpp"
#include "dhu_sdp.h"
#include "sdr_builder.hpp"

namespace aa {
namespace {

const unsigned kPingsLogged = 5;  // then one in kPingsLogEvery
const unsigned kPingsLogEvery = 300;
const unsigned kNavigationLogEvery = 50;
const unsigned kMediaLogEvery = 30;
const unsigned kAudioLoggedFirst = 3;
const unsigned kAudioLogEvery = 500;

int signedOrMinusOne(uint32_t value) {
    return value == kNoField ? -1 : static_cast<int>(value);
}

uint64_t bigEndian64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

const uint8_t* bodyOf(const Message& message) {
    return message.size() > kMessageIdSize ? &message[kMessageIdSize] : nullptr;
}

size_t bodySize(const Message& message) {
    return message.size() > kMessageIdSize ? message.size() - kMessageIdSize : 0;
}

}  // namespace

bool AASession::dispatch(uint8_t channel, const Message& message) {
    const uint16_t id = messageId(message);
    g_trail.stage = kTrailHandler;
    g_trail.message = id;
    switch (routeOf(channel, id)) {
        case Route::kSensorStart:
            onSensorStart(channel, message);
            break;
        case Route::kMicrophoneOpen:
            onMicrophoneOpen(channel, message);
            break;
        case Route::kMicrophoneAck:  // acks for our microphone audio: nothing to do
            break;
        case Route::kChannelOpen:
            onChannelOpen(channel);
            break;
        case Route::kNavigationStatus:
            onNavigationStatus(id, message);
            break;
        case Route::kPlaybackStatus:
            onPlaybackStatus(id, message);
            break;
        case Route::kSetup:
            onSetup(channel);
            break;
        case Route::kStart:
            onStart(channel, message);
            break;
        case Route::kAudioStop:
            printf("[AASession] >>> Received AV STOP_INDICATION on Channel %u.\n", channel);
            break;
        case Route::kInputBinding:
            onInputBinding(channel);
            break;
        case Route::kVideoFocusRequest:
            onVideoFocusRequest(channel, message);
            break;
        case Route::kVideoFocusNotice:
            printf("[AASession] >>> Received VideoFocusIndication on Channel %u.\n", channel);
            break;
        case Route::kMedia:
            onMedia(channel, id, message);
            break;
        case Route::kControl:
            return onControl(id, message);
        case Route::kOther:
            printf("[AASession] Channel %u Message: ID=0x%04X, Len=%u\n", channel, id,
                   static_cast<unsigned>(message.size()));
            break;
    }
    return true;
}

// ---- control channel ----------------------------------------------------------------------------------------------

bool AASession::onControl(uint16_t id, const Message& message) {
    switch (id) {
        case msg::kVersionResponse:
            onVersion(message);
            break;
        case msg::kTlsHandshake:
            onTlsHandshake(message);
            break;
        case msg::kDiscoveryRequest:
            onDiscovery();
            break;
        case msg::kAudioFocusRequest:
            onAudioFocus(message);
            break;
        case msg::kNavigationFocusRequest:
            onNavigationFocus(message);
            break;
        case msg::kVoiceSessionRequest: {
            const uint32_t type = bodyField(message, 1);  // 1 START, 2 STOP: nothing to do either way
            printf("[AASession] >>> VoiceSessionRequest: %s\n", type == 1 ? "START" : type == 2 ? "STOP" : "?");
            break;
        }
        case msg::kByeByeRequest:
            onByeBye(message);
            return false;
        case msg::kPingRequest:
            onPing(message);
            break;
        case msg::kPingResponse:  // to our own ping: like any data, it already reset the silence
            if (pingAnswers_++ < kPingsLogged || pingAnswers_ % kPingsLogEvery == 0) {
                printf("[AASession] >>> PingResponse %u to ours\n", pingAnswers_);
            }
            break;
        default:
            printf("[AASession] Control Message RX: ID=0x%04X, Len=%u\n", id, static_cast<unsigned>(message.size()));
            break;
    }
    return true;
}

// The phone answered our version request: TLS starts, the head unit speaking first.
void AASession::onVersion(const Message& message) {
    if (message.size() < 8) {
        return;
    }
    printf("[AASession] >>> VersionResponse: Major=%u, Minor=%u, Status=%u\n", (message[2] << 8) | message[3],
           (message[4] << 8) | message[5], (message[6] << 8) | message[7]);
    std::vector<uint8_t> hello;
    if (tls_.start() && tls_.hello(hello)) {
        sendPlain(channel::kControl, message::tlsHandshake(hello));
        printf("[AASession] Sent TLS ClientHello with the head unit certificate.\n");
    }
}

void AASession::onTlsHandshake(const Message& message) {
    if (message.size() < kMessageIdSize + 1) {
        printf("[AASession] Ignoring empty SSL handshake message.\n");
        return;
    }
    printf("[AASession] >>> Received SSL Handshake from phone (%u bytes)\n", static_cast<unsigned>(bodySize(message)));
    std::vector<uint8_t> answer;
    tls_.handshake(bodyOf(message), bodySize(message), answer);
    if (!answer.empty()) {
        sendPlain(channel::kControl, message::tlsHandshake(answer));
        printf("[AASession] Sent TLS Handshake response (%u bytes).\n", static_cast<unsigned>(answer.size()));
    }
    if (tls_.established()) {
        // AuthComplete goes in clear, as every head unit sends it.
        printf("[AASession] >>> TLS Handshake Complete! Sending AuthCompleteNotification...\n");
        sendPlain(channel::kControl, message::authComplete());
        printf("[AASession] >>> Awaiting ServiceDiscoveryRequest from phone...\n");
    }
}

// The description of this car: Google's Desktop Head Unit template, adapted by core/sdr_builder.cpp.
void AASession::onDiscovery() {
    printf("[AASession] >>> Received ServiceDiscoveryRequest from phone! Sending ServiceDiscoveryResponse...\n");
    const Message response = message::discoveryResponse(
        buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, vehicleFile_, softwareVersion_, display_,
                                 carMake_, codedSide_, codedModel_));
    if (sendSealed(channel::kControl, response)) {
        printf("[AASession] >>> Sent ServiceDiscoveryResponse (%u bytes)\n", static_cast<unsigned>(response.size()));
    }
}

// Asked 1 GAIN, 2 GAIN_TRANSIENT, 3 GAIN_NAVI (guidance prompts) or 4 RELEASE: RELEASE gets LOSS, the rest GAIN.
// Answering RELEASE with NONE left the phone without a clean end of each guidance prompt.
void AASession::onAudioFocus(const Message& message) {
    // The byte after the field key, as read since the first car tests.
    const uint32_t asked = message.size() >= 4 ? message[3] : 0;
    const uint32_t granted = asked == focus::kAudioRelease ? focus::kAudioLoss : focus::kAudioGain;
    printf("[AASession] >>> AudioFocusRequest (request=%u): AudioFocusResponse(state=%u)\n", asked, granted);
    sendSealed(channel::kControl, message::audioFocusResponse(granted));
}

// Maps asks when guidance starts and ends; without an answer it never gets it. Always granted: this head unit runs no
// guidance of its own.
void AASession::onNavigationFocus(const Message& message) {
    const uint32_t asked = bodyField(message, 1);
    const bool sent = sendSealed(channel::kControl, message::navigationFocusResponse());
    printf("[AASession] >>> NavigationFocusRequest (type=%d) -> NavigationFocusResponse(PROJECTED) %s\n",
           signedOrMinusOne(asked), sent ? "sent" : "SEND FAILED");
}

// The phone ends the session: answered, then the session ends here. Left unanswered while waiting for the phone to
// close, the phone was seen sending ByeBye right after the discovery and reconnecting every 3 s.
void AASession::onByeBye(const Message& message) {
    const uint32_t reason = bodyField(message, 1);
    goodbye_.store(true);  // before the session ends: the ended callback reads it
    const bool sent = sendSealed(channel::kControl, message::byeByeResponse());
    printf("[AASession] >>> ByeByeRequest (reason=%d%s, %u media messages): ByeByeResponse %s, ending the session\n",
           signedOrMinusOne(reason), reason == 1 ? " = quit" : "", mediaMessages_, sent ? "sent" : "SEND FAILED");
}

void AASession::onPing(const Message& message) {
    if (pings_++ < kPingsLogged || pings_ % kPingsLogEvery == 0) {
        printf("[AASession] >>> PingRequest %u, PingResponse sent\n", pings_);
    }
    sendPlain(channel::kControl, message::pingResponse(message));  // in clear, whatever the request
}

// Sent by the phone on the channel it opens, and answered there.
void AASession::onChannelOpen(uint8_t channel) {
    printf("[AASession] >>> ChannelOpenRequest on Channel %u: ChannelOpenResponse (SUCCESS)\n", channel);
    sendSealed(channel, message::channelOpenResponse(), frame::kSealedChannelControl);
}

// ---- sensor channel -----------------------------------------------------------------------------------------------

void AASession::onSensorStart(uint8_t channel, const Message& message) {
    const uint32_t type = bodyField(message, 1);
    printf("[AASession] >>> SensorStartRequest (type=%u)\n", type);
    sendSealed(channel, message::sensorStartResponse(true));
    if (type == sensor::kNightMode) {
        inbox_.startNightMode();
    }
    if (type == sensor::kLocation) {
        const uint32_t periodMs = bodyField(message, 2);
        inbox_.startLocation(periodMs == kNoField ? sensor::kDefaultPeriodMs : periodMs);
        printf("[AASession] location: the phone subscribed to the car's position\n");
    }
    if (type != kNoField) {
        sendSensorEvent(type);
    }
}

// ---- audio and video channels -------------------------------------------------------------------------------------

// Only the main display gets the focus: focusing every display of the template makes the phone encode several
// H.264 streams, and only channel 6 is shown.
void AASession::onSetup(uint8_t channel) {
    printf("[AASession] >>> AV SETUP_REQUEST on Channel %u: SETUP_RESPONSE%s\n", channel,
           channel == channel::kVideo ? " and VIDEO_FOCUS" : "");
    if (sendSealed(channel, message::setupResponse())) {
        printf("[AASession] >>> Sent AV SETUP_RESPONSE on Channel %u (max_unacked=%u)!\n", channel, kMaxUnacked);
    }
    if (channel == channel::kVideo) {
        sendVideoFocus(true, false);
        videoReady_ = true;
        keyframe_.videoStarted(locationClockMs());
    }
}

// Our acks must carry the session id of the channel's start indication. It is 0 the first time (video, media), but a
// stream stopped and started again (guidance, Assistant) gets a new one, and acks with 0 then fill the phone's window
// of unacked messages: the stream stalls.
void AASession::onStart(uint8_t channel, const Message& message) {
    const uint32_t session = bodyField(message, 1);
    if (session != kNoField) {
        mediaSessions_[channel & 31] = session;
    }
    printf("[AASession] >>> AV START_INDICATION on Channel %u (session %u)\n", channel,
           session == kNoField ? 0u : session);
}

// Media of any channel: counted, video handed to the callbacks, everything acked. The phone counts each media message
// it sends against its window of kMaxUnacked until acked, whether the head unit could use it or not: video cut too
// short to hold a picture is acked too, or it would hold a slot of that window for the rest of the session.
void AASession::onMedia(uint8_t channel, uint16_t id, const Message& message) {
    ++mediaMessages_;
    if (mediaMessages_ % kMediaLogEvery == 1) {
        printf("[AASession] >>> Streaming Media Frame %u on Channel %u (%u bytes)\n", mediaMessages_, channel,
               static_cast<unsigned>(message.size()));
    }
    if (channel::isAudioSink(channel) && id == msg::kMediaData) {
        // Sound plays over the car's Bluetooth: phone audio is only counted (8-byte timestamp, then s16le PCM).
        if (message.size() > kTimestampedHeader) {
            uint32_t& count = audioMessages_[channel == channel::kMediaAudio ? 1 : 0];
            ++count;
            if (count <= kAudioLoggedFirst || count % kAudioLogEvery == 0) {
                printf("[AASession] audio %s frame #%u: %u bytes PCM, pts=%llu\n",
                       channel == channel::kMediaAudio ? "media(ch9)" : "speech(ch8)", count,
                       static_cast<unsigned>(message.size() - kTimestampedHeader),
                       static_cast<unsigned long long>(bigEndian64(&message[kMessageIdSize])));
            }
        }
    } else if (channel == channel::kVideo) {
        // Cut at the protocol's offsets, never by scanning: a timestamp can hold 00 00 01 and look like a start code.
        const size_t offset = id == msg::kMediaConfig ? kMessageIdSize : kTimestampedHeader;
        if (message.size() <= offset) {
            // Too short to carry any picture: no callback, but acked below like every other media message.
            printf("[AASession] Dropping truncated/empty video message 0x%04X (%u bytes).\n", id,
                   static_cast<unsigned>(message.size()));
        } else if (id == msg::kMediaConfig) {
            const size_t configBytes = message.size() - offset;
            printf("[AASession] >>> Received H.264 SPS/PPS codec config (%u bytes)!\n",
                   static_cast<unsigned>(configBytes));
            if (codec_.fn) {
                codec_.fn(&message[offset], configBytes, codec_.user);
            }
        } else if (video_.fn) {
            // The callback owns the decoder feed, SPS/PPS before the next IDR included.
            g_trail.stage = kTrailVideo;
            video_.fn(mediaMessages_, &message[offset], message.size() - offset, bigEndian64(&message[kMessageIdSize]),
                      video_.user);
            g_trail.stage = kTrailHandler;
        }
    }
    sendSealed(channel, message::mediaAck(mediaSessions_[channel & 31]));
}

// A message the phone sent on one of its streams, but lost on the way (a fragment that did not decrypt, a reassembly
// that failed): acked once, as onMedia acks those it gets, so that the phone's window does not shrink by one for good;
// ten such losses would stop the stream. The ack goes with the LAST fragment, which closes the message and keeps its
// position in the clear header even when it does not decrypt. On these streams the phone fragments only media: its
// other messages there are a few bytes. A whole frame that did not decrypt is not acked: it may have been one of them.
void AASession::ackDropped(const FrameHeader& header) {
    if (header.position() != frame::kLast || !channel::isMediaSink(header.channel)) {
        return;
    }
    const bool sent = sendSealed(header.channel, message::mediaAck(mediaSessions_[header.channel & 31]));
    printf("[AASession] media message lost on the way, acked on channel %u %s\n", header.channel,
           sent ? "sent" : "SEND FAILED");
}

// The phone's "exit to car" icon asks for the native screen; the same answer is given and the callback hides the
// picture. A request for the projection is granted the same way.
void AASession::onVideoFocusRequest(uint8_t channel, const Message& message) {
    const uint32_t mode = bodyField(message, 2);
    const uint32_t reason = bodyField(message, 3);
    const bool projected = mode != focus::kNative;
    printf("[AASession] >>> VideoFocusRequest on Channel %u: mode=%u (%s) reason=%u\n", channel, mode,
           projected ? "projected" : "native / exit to car", reason);
    sendVideoFocus(projected, false);
    if (focus_.fn) {
        focus_.fn(projected, reason, focus_.user);
    }
}

// ---- input channels -----------------------------------------------------------------------------------------------

// Accepted even with touch forwarding off: keys, the microphone wake-up and the video focus share channel 7, and the
// phone waits for this answer.
void AASession::onInputBinding(uint8_t channel) {
    printf("[AASession] >>> Input BindingRequest on Channel %u: accepting.\n", channel);
    sendSealed(channel, message::bindingResponse());
    if (channel == channel::kInput) {
        inbox_.bindInput();
        printf("[AASession] Touch forwarding armed.\n");
    }
}

// ---- microphone channel -------------------------------------------------------------------------------------------

// {1 open, 2 noise reduction, 3 echo cancellation, 4 max unacked}
void AASession::onMicrophoneOpen(uint8_t channel, const Message& message) {
    const bool open = bodyField(message, 1) == 1;
    printf("[AASession] >>> Microphone %s request (max_unacked=%d)\n", open ? "OPEN" : "CLOSE",
           signedOrMinusOne(bodyField(message, 4)));
    inbox_.setMicrophoneOpen(open);
    if (open) {
        microphoneChunks_ = 0;
        sendSealed(channel, message::micOpenResponse());
    }
    if (microphone_.fn) {
        microphone_.fn(open, microphone_.user);
    }
}

// ---- status channels ----------------------------------------------------------------------------------------------

// Guidance state, next turn, distance...: nothing is answered, all goes to the callback (the cluster arrows).
void AASession::onNavigationStatus(uint16_t id, const Message& message) {
    if ((navigationLogged_++ % kNavigationLogEvery) == 0 || id == msg::kGuidanceState) {
        printf("[AASession] nav status ch10: msg 0x%04X (%u bytes)%s\n", id, static_cast<unsigned>(message.size()),
               id != msg::kGuidanceState    ? ""
               : bodyField(message, 1) == 1 ? " guidance ACTIVE"
                                            : " guidance inactive");
    }
    if (navigation_.fn) {
        navigation_.fn(id, bodyOf(message), bodySize(message), navigation_.user);
    }
}

// Playback state every second, track metadata and album art: only changes of state are logged.
void AASession::onPlaybackStatus(uint16_t id, const Message& message) {
    if (id != msg::kPlaybackState) {
        return;
    }
    const uint32_t state = bodyField(message, 1);
    if (state != playbackState_) {
        playbackState_ = state;
        printf("[AASession] media status ch12: state %u\n", state);
    }
}

}  // namespace aa
