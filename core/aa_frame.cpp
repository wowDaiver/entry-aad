// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto frame header and fragment reassembly (tests/test_aa_frame.cpp).
#include "aa_frame.hpp"

namespace aa {

FrameHeader decodeFrameHeader(const uint8_t* bytes) {
    FrameHeader header;
    header.channel = bytes[0];
    header.flags = bytes[1];
    header.length = static_cast<uint16_t>((bytes[2] << 8) | bytes[3]);
    header.totalSize = 0;
    return header;
}

uint32_t decodeTotalSize(const uint8_t* bytes) {
    uint32_t size = 0;
    for (int i = 0; i < 4; ++i) {
        size = (size << 8) | bytes[i];
    }
    return size;
}

bool encodeFrame(uint8_t channel, uint8_t flags, const uint8_t* payload, size_t length, std::vector<uint8_t>& out) {
    if (length > frame::kMaxPayload || (length > 0 && !payload)) {
        return false;
    }
    out.clear();
    out.reserve(frame::kHeaderSize + length);
    out.push_back(channel);
    out.push_back(flags);
    out.push_back(static_cast<uint8_t>(length >> 8));
    out.push_back(static_cast<uint8_t>(length & 0xFF));
    if (length > 0) {
        out.insert(out.end(), payload, payload + length);
    }
    return true;
}

MessageAssembler::Outcome MessageAssembler::add(const FrameHeader& header, const std::vector<uint8_t>& payload,
                                                std::vector<uint8_t>& message) {
    const uint8_t position = header.position();
    if (position == frame::kWhole) {
        // A message of its own, one frame long: what is being assembled or skipped on the channel stays as it is.
        if (payload.size() > frame::kMaxMessage) {
            return kOverflow;
        }
        message = payload;
        return kComplete;
    }
    if (position == frame::kFirst) {
        // Replaces an unfinished message, or ends the tail of a lost one whose LAST never came.
        if (payload.size() > frame::kMaxMessage) {
            forget(header.channel, position);
            return kOverflow;
        }
        Pending& pending = pending_[header.channel];
        pending.data = payload;
        pending.lostTail = false;
        pending.size = header.totalSize;
        pending.onWire = header.length;
        return kPending;
    }
    const std::map<uint8_t, Pending>::iterator it = pending_.find(header.channel);
    if (it == pending_.end()) {
        forget(header.channel, position);
        return kNoFirst;
    }
    Pending& pending = it->second;
    if (pending.lostTail) {
        if (position == frame::kLast) {
            pending_.erase(it);
        }
        return kRestOfLost;
    }
    if (payload.size() > frame::kMaxMessage - pending.data.size()) {
        forget(header.channel, position);
        return kOverflow;
    }
    if (position == frame::kMiddle) {
        pending.data.insert(pending.data.end(), payload.begin(), payload.end());
        pending.onWire += header.length;
        return kPending;
    }
    // LAST closes the message started by FIRST.
    const uint32_t onWire = pending.onWire + header.length;
    const uint32_t size = pending.size;
    pending.data.insert(pending.data.end(), payload.begin(), payload.end());
    message.swap(pending.data);
    pending_.erase(it);
    // The announced size counts plain bytes and the length fields count TLS records, which are larger: only a
    // shortfall tells that this LAST closed an incomplete message.
    if (size != 0 && onWire < size) {
        received_ = onWire;
        announced_ = size;
        return kShortOfSize;
    }
    return kComplete;
}

void MessageAssembler::lose(const FrameHeader& header) {
    if (header.position() != frame::kWhole) {
        forget(header.channel, header.position());
    }
}

// Erased first, so that what was gathered is freed; a LAST leaves nothing of its message to skip.
void MessageAssembler::forget(uint8_t channel, uint8_t position) {
    pending_.erase(channel);
    if (position != frame::kLast) {
        pending_[channel].lostTail = true;
    }
}

}  // namespace aa
