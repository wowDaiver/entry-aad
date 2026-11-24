// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto frames: the header both ways, and the reassembly of fragmented messages. No I/O, no logging.
#ifndef ENTRY_AAD_CORE_AA_FRAME_HPP
#define ENTRY_AAD_CORE_AA_FRAME_HPP

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <vector>

#include "aa_protocol.hpp"

namespace aa {

struct FrameHeader {
    uint8_t channel;
    uint8_t flags;
    uint16_t length;     // payload bytes on the wire (TLS records when encrypted)
    uint32_t totalSize;  // a FIRST fragment announces its message's size; 0 otherwise

    uint8_t position() const { return flags & frame::kPositionMask; }
    bool encrypted() const { return (flags & frame::kEncrypted) != 0; }
    // A FIRST fragment carries 4 more header bytes, the total size.
    bool announcesSize() const { return position() == frame::kFirst; }
};

// The 4 fixed header bytes; totalSize stays 0.
FrameHeader decodeFrameHeader(const uint8_t* bytes);
// The 4 bytes that follow the header of a FIRST fragment.
uint32_t decodeTotalSize(const uint8_t* bytes);
// A whole frame as the head unit sends it (never fragmented). False when the payload exceeds 64 KiB - 1.
bool encodeFrame(uint8_t channel, uint8_t flags, const uint8_t* payload, size_t length, std::vector<uint8_t>& out);

// Joins the fragments of each channel's messages. A whole frame passes straight through and leaves a message being
// assembled on its channel as it is. A message that cannot be rebuilt is dropped once: when that is known before its
// LAST, the rest of its fragments are skipped quietly up to that LAST, rather than each dropped on its own or taken for
// the start of a message.
class MessageAssembler {
public:
    enum Outcome {
        kPending,      // a FIRST or MIDDLE fragment, kept
        kComplete,     // `message` holds a whole message
        kOverflow,     // the channel's message would pass frame::kMaxMessage: dropped
        kNoFirst,      // a MIDDLE or LAST fragment without a FIRST on its channel: its message is dropped
        kShortOfSize,  // a LAST fragment closed a message smaller than announced: dropped
        kRestOfLost,   // a MIDDLE or LAST fragment of a message dropped before: nothing more to say about it
    };

    MessageAssembler() : received_(0), announced_(0) {}

    // `payload` is the frame's decrypted payload; `header.length` counts what came on the wire.
    Outcome add(const FrameHeader& header, const std::vector<uint8_t>& payload, std::vector<uint8_t>& message);
    // A frame whose payload could not be decrypted: the message it belongs to is lost, as by add(). A whole frame
    // belongs to no message being assembled and changes nothing here.
    void lose(const FrameHeader& header);
    void clear() { pending_.clear(); }
    // After kShortOfSize: the wire bytes received and the size the phone announced.
    uint32_t receivedBytes() const { return received_; }
    uint32_t announcedBytes() const { return announced_; }

private:
    // The message of a fragment cannot be rebuilt: after a FIRST or MIDDLE, the rest of it is skipped.
    void forget(uint8_t channel, uint8_t position);

    struct Pending {
        std::vector<uint8_t> data;
        bool lostTail;    // the message was lost: its fragments up to its LAST are ignored
        uint32_t size;    // the size its FIRST announced
        uint32_t onWire;  // the length fields received so far
        Pending() : lostTail(false), size(0), onWire(0) {}
    };
    std::map<uint8_t, Pending> pending_;
    uint32_t received_;
    uint32_t announced_;
};

}  // namespace aa

#endif  // ENTRY_AAD_CORE_AA_FRAME_HPP
