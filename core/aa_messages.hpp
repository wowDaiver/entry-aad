// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The messages the head unit sends to the phone, as bytes: the 2-byte id, then the body. No state, no I/O.
#ifndef ENTRY_AAD_CORE_AA_MESSAGES_HPP
#define ENTRY_AAD_CORE_AA_MESSAGES_HPP

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "aa_input.hpp"

namespace aa {

typedef std::vector<uint8_t> Message;

// Message id of a received message; 0 (media data) when it is shorter than an id, as the dispatch has always read it.
uint16_t messageId(const Message& message);
// A varint field of a received message's body, cut to 32 bits; kNoField when missing or unreadable.
const uint32_t kNoField = 0xFFFFFFFFu;
uint32_t bodyField(const Message& message, uint32_t number);

namespace message {

// Control channel.
Message versionRequest();
Message tlsHandshake(const std::vector<uint8_t>& records);
Message authComplete();
Message discoveryResponse(const std::vector<uint8_t>& description);
Message channelOpenResponse();
// Echoes the request's timestamp bytes as they came, when the request starts with that field.
Message pingResponse(const Message& request);
// The head unit's own ping, carrying its clock; the phone answers with a ping response.
Message pingRequest(uint64_t timestampUs);
Message audioFocusResponse(uint32_t state);
Message navigationFocusResponse();
Message byeByeResponse();

// Audio, video and microphone channels.
Message setupResponse();
Message videoFocusIndication(bool projected, bool unrequested);
Message mediaAck(uint32_t session);
Message micOpenResponse();
Message micData(uint64_t timestampUs, const std::vector<uint8_t>& pcm);

// Sensor channel (the location messages are in core/car_location.hpp).
Message sensorStartResponse(bool ok);
Message nightModeEvent(bool night);
Message drivingStatusEvent(uint32_t status);

// Input channel.
Message bindingResponse();
Message keyEvent(uint64_t timestampUs, uint32_t keycode, bool down);
Message rotaryEvent(uint64_t timestampUs, uint32_t keycode, int delta);
Message touchEvent(uint64_t timestampUs, const TouchEventOut& touch);

}  // namespace message
}  // namespace aa

#endif  // ENTRY_AAD_CORE_AA_MESSAGES_HPP
