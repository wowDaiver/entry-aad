// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Builders of the head unit's messages on core/pb.hpp, and the two readers the session uses on the phone's.
#include "aa_messages.hpp"

#include "aa_protocol.hpp"
#include "pb.hpp"

namespace aa {
namespace {

// Field numbers of the bodies built here.
const uint32_t kStatusField = 1;     // the status of every plain response
const uint32_t kTimestampField = 1;  // ping and input events
const uint32_t kSetupMaxUnackedField = 2, kSetupConfigField = 3;
const uint32_t kFocusModeField = 1, kFocusUnrequestedField = 2;
const uint32_t kAckSessionField = 1, kAckCountField = 2;
const uint32_t kMicSessionField = 1, kMicStatusField = 2;
const uint32_t kSensorValueField = 1;  // the single value of NightMode and DrivingStatus
// InputEventIndication: the touch, key and relative events, and the fields inside them.
const uint32_t kTouchField = 3, kKeysField = 4, kRelativeField = 6;
const uint32_t kEventListField = 1;  // repeated ButtonEvent / RelativeInputEvent
const uint32_t kKeyCodeField = 1, kKeyDownField = 2, kKeyMetaField = 3, kKeyLongPressField = 4;
const uint32_t kRelativeCodeField = 1, kRelativeDeltaField = 2;
const uint32_t kPointerField = 1, kPointerIndexField = 2, kTouchActionField = 3;
const uint32_t kPointerXField = 1, kPointerYField = 2, kPointerIdField = 3;

// A ping response echoes the timestamp bytes; it never grows past this.
const size_t kPingResponseMax = 16;
const uint8_t kTimestampKey = (kTimestampField << 3) | pb::kVarint;

Message withId(uint16_t id) {
    Message m;
    m.push_back(static_cast<uint8_t>(id >> 8));
    m.push_back(static_cast<uint8_t>(id & 0xFF));
    return m;
}

void appendBigEndian(Message& m, uint64_t value, int bytes) {
    for (int shift = 8 * (bytes - 1); shift >= 0; shift -= 8) {
        m.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

Message statusOnly(uint16_t id, uint32_t status) {
    Message m = withId(id);
    pb::appendUint(m, kStatusField, status);
    return m;
}

Message inputEvent(uint64_t timestampUs, uint32_t kind, const Message& event) {
    Message m = withId(msg::kInputEvent);
    pb::appendUint(m, kTimestampField, timestampUs);
    pb::appendMessage(m, kind, event);
    return m;
}

}  // namespace

uint16_t messageId(const Message& message) {
    return message.size() >= kMessageIdSize ? static_cast<uint16_t>((message[0] << 8) | message[1]) : 0;
}

uint32_t bodyField(const Message& message, uint32_t number) {
    uint64_t value = 0;
    if (message.size() <= kMessageIdSize ||
        !pb::findVarint(&message[kMessageIdSize], message.size() - kMessageIdSize, number, value)) {
        return kNoField;
    }
    return static_cast<uint32_t>(value);
}

namespace message {

Message versionRequest() {
    Message m = withId(msg::kVersionRequest);
    appendBigEndian(m, kVersionMajor, 2);
    appendBigEndian(m, kVersionMinor, 2);
    appendBigEndian(m, 0, 2);  // status
    return m;
}

Message tlsHandshake(const std::vector<uint8_t>& records) {
    Message m = withId(msg::kTlsHandshake);
    m.insert(m.end(), records.begin(), records.end());
    return m;
}

Message authComplete() {
    return statusOnly(msg::kAuthComplete, kStatusOk);
}

Message discoveryResponse(const std::vector<uint8_t>& description) {
    Message m = withId(msg::kDiscoveryResponse);
    m.insert(m.end(), description.begin(), description.end());
    return m;
}

Message channelOpenResponse() {
    return statusOnly(msg::kChannelOpenResponse, kStatusOk);
}

Message pingResponse(const Message& request) {
    Message m = withId(msg::kPingResponse);
    if (request.size() > kMessageIdSize + 1 && request[kMessageIdSize] == kTimestampKey) {
        m.push_back(kTimestampKey);
        for (size_t i = kMessageIdSize + 1; i < request.size() && m.size() < kPingResponseMax; ++i) {
            m.push_back(request[i]);
            if (!(request[i] & 0x80)) {
                break;
            }
        }
    }
    return m;
}

Message pingRequest(uint64_t timestampUs) {
    Message m = withId(msg::kPingRequest);
    pb::appendUint(m, kTimestampField, timestampUs);
    return m;
}

Message audioFocusResponse(uint32_t state) {
    return statusOnly(msg::kAudioFocusResponse, state);
}

Message navigationFocusResponse() {
    return statusOnly(msg::kNavigationFocusResponse, focus::kNavigationProjected);
}

Message byeByeResponse() {
    return withId(msg::kByeByeResponse);
}

Message setupResponse() {
    Message m = statusOnly(msg::kSetupResponse, kMediaStatusOk);
    pb::appendUint(m, kSetupMaxUnackedField, kMaxUnacked);
    pb::appendUint(m, kSetupConfigField, 0);
    return m;
}

Message videoFocusIndication(bool projected, bool unrequested) {
    Message m = withId(msg::kVideoFocusIndication);
    pb::appendUint(m, kFocusModeField, projected ? focus::kProjected : focus::kNative);
    pb::appendUint(m, kFocusUnrequestedField, unrequested ? 1 : 0);
    return m;
}

Message mediaAck(uint32_t session) {
    Message m = withId(msg::kMediaAck);
    pb::appendUint(m, kAckSessionField, session);
    pb::appendUint(m, kAckCountField, 1);
    return m;
}

Message micOpenResponse() {
    Message m = withId(msg::kMicOpenResponse);
    pb::appendUint(m, kMicSessionField, 0);
    pb::appendUint(m, kMicStatusField, kStatusOk);
    return m;
}

Message micData(uint64_t timestampUs, const std::vector<uint8_t>& pcm) {
    Message m = withId(msg::kMediaData);
    m.reserve(kTimestampedHeader + pcm.size());
    appendBigEndian(m, timestampUs, 8);
    m.insert(m.end(), pcm.begin(), pcm.end());
    return m;
}

Message sensorStartResponse(bool ok) {
    return statusOnly(msg::kSensorStartResponse, ok ? kStatusOk : 1);
}

Message nightModeEvent(bool night) {
    Message value;
    pb::appendUint(value, kSensorValueField, night ? 1 : 0);
    Message m = withId(msg::kSensorEvent);
    pb::appendMessage(m, sensor::kNightMode, value);
    return m;
}

Message drivingStatusEvent(uint32_t status) {
    Message value;
    pb::appendUint(value, kSensorValueField, status);
    Message m = withId(msg::kSensorEvent);
    pb::appendMessage(m, sensor::kDrivingStatus, value);
    return m;
}

Message bindingResponse() {
    return statusOnly(msg::kBindingResponse, kStatusOk);
}

Message keyEvent(uint64_t timestampUs, uint32_t keycode, bool down) {
    Message key;
    pb::appendUint(key, kKeyCodeField, keycode);
    pb::appendUint(key, kKeyDownField, down ? 1 : 0);
    pb::appendUint(key, kKeyMetaField, 0);
    pb::appendUint(key, kKeyLongPressField, 0);
    Message keys;
    pb::appendMessage(keys, kEventListField, key);
    return inputEvent(timestampUs, kKeysField, keys);
}

Message rotaryEvent(uint64_t timestampUs, uint32_t keycode, int delta) {
    Message step;
    pb::appendUint(step, kRelativeCodeField, keycode);
    pb::appendUint(step, kRelativeDeltaField, static_cast<uint64_t>(static_cast<int64_t>(delta)));
    Message steps;
    pb::appendMessage(steps, kEventListField, step);
    return inputEvent(timestampUs, kRelativeField, steps);
}

Message touchEvent(uint64_t timestampUs, const TouchEventOut& touch) {
    // Coordinates and ids go out as 32-bit values: a negative one is a 5-byte varint, as the phones have always seen.
    Message event;
    for (int i = 0; i < touch.n; ++i) {
        Message pointer;
        pb::appendUint(pointer, kPointerXField, static_cast<uint32_t>(touch.p[i].x));
        pb::appendUint(pointer, kPointerYField, static_cast<uint32_t>(touch.p[i].y));
        pb::appendUint(pointer, kPointerIdField, static_cast<uint32_t>(touch.p[i].id));
        pb::appendMessage(event, kPointerField, pointer);
    }
    pb::appendUint(event, kPointerIndexField, static_cast<uint32_t>(touch.index));
    pb::appendUint(event, kTouchActionField, static_cast<uint32_t>(touch.action));
    return inputEvent(timestampUs, kTouchField, event);
}

}  // namespace message
}  // namespace aa
