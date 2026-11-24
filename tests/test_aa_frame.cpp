// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto frame header and fragment reassembly (core/aa_frame.hpp), on the PC.
#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include "aa_frame.hpp"

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                            \
        }                                                       \
    } while (0)

namespace {

typedef std::vector<uint8_t> Bytes;

aa::FrameHeader header(uint8_t channel, uint8_t position, uint16_t length, uint32_t total = 0) {
    aa::FrameHeader h;
    h.channel = channel;
    h.flags = static_cast<uint8_t>(position | aa::frame::kEncrypted);
    h.length = length;
    h.totalSize = total;
    return h;
}

void headers() {
    const uint8_t first[] = {0x06, 0x09, 0x12, 0x34, 0x00, 0x01, 0x02, 0x03};
    const aa::FrameHeader h = aa::decodeFrameHeader(first);
    CHECK(h.channel == 6 && h.flags == 0x09 && h.length == 0x1234 && h.totalSize == 0);
    CHECK(h.announcesSize() && h.encrypted() && h.position() == aa::frame::kFirst);
    CHECK(aa::decodeTotalSize(first + 4) == 0x00010203u);
    const uint8_t whole[] = {0x00, 0x03, 0x00, 0x08};
    CHECK(!aa::decodeFrameHeader(whole).announcesSize() && !aa::decodeFrameHeader(whole).encrypted());

    Bytes out;
    const uint8_t body[] = {0xaa, 0xbb};
    CHECK(aa::encodeFrame(7, 0x0b, body, 2, out) && out == Bytes({7, 0x0b, 0x00, 0x02, 0xaa, 0xbb}));
    CHECK(aa::encodeFrame(0, 0x03, nullptr, 0, out) && out == Bytes({0, 0x03, 0, 0}));
    CHECK(!aa::encodeFrame(0, 0x03, nullptr, 1, out));
    const Bytes biggest(0xFFFF, 1);
    CHECK(aa::encodeFrame(1, 0x0b, &biggest[0], biggest.size(), out) && out.size() == 0xFFFF + 4 && out[2] == 0xFF);
    const Bytes tooBig(0x10000, 1);
    CHECK(!aa::encodeFrame(1, 0x0b, &tooBig[0], tooBig.size(), out));
}

void reassembly() {
    aa::MessageAssembler a;
    Bytes m;
    CHECK(a.add(header(6, aa::frame::kWhole, 9), Bytes({1, 2}), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes({1, 2}));

    // FIRST, MIDDLE, LAST: the wire lengths (TLS records) are larger than the announced plain size.
    CHECK(a.add(header(6, aa::frame::kFirst, 30, 6), Bytes({1, 2}), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kMiddle, 30), Bytes({3, 4}), m) == aa::MessageAssembler::kPending);
    // A whole message on the same channel passes through and leaves the fragments as they are.
    CHECK(a.add(header(6, aa::frame::kWhole, 30), Bytes({9}), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes({9}));
    CHECK(a.add(header(8, aa::frame::kFirst, 30, 2), Bytes({7}), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kLast, 30), Bytes({5, 6}), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes({1, 2, 3, 4, 5, 6}));
    CHECK(a.add(header(8, aa::frame::kLast, 30), Bytes({8}), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes({7, 8}));

    // Neither continuation starts a message, even if another channel has a FIRST in flight. After an orphan MIDDLE,
    // the rest of its message is skipped up to its LAST: that message is dropped once, not once per fragment.
    CHECK(a.add(header(8, aa::frame::kFirst, 30, 2), Bytes({7}), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kLast, 30), Bytes({1}), m) == aa::MessageAssembler::kNoFirst);
    CHECK(a.add(header(6, aa::frame::kMiddle, 30), Bytes({1}), m) == aa::MessageAssembler::kNoFirst);
    CHECK(a.add(header(6, aa::frame::kMiddle, 30), Bytes({2}), m) == aa::MessageAssembler::kRestOfLost);
    CHECK(a.add(header(6, aa::frame::kLast, 30), Bytes({3}), m) == aa::MessageAssembler::kRestOfLost);
    CHECK(a.add(header(6, aa::frame::kLast, 30), Bytes({4}), m) == aa::MessageAssembler::kNoFirst);
    CHECK(a.add(header(8, aa::frame::kLast, 30), Bytes({8}), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes({7, 8}));

    // Announced more than the wire brought: dropped. Announced 0: no check.
    CHECK(a.add(header(6, aa::frame::kFirst, 10, 100), Bytes({1}), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kLast, 20), Bytes({2}), m) == aa::MessageAssembler::kShortOfSize);
    CHECK(a.receivedBytes() == 30 && a.announcedBytes() == 100);
    CHECK(a.add(header(6, aa::frame::kFirst, 10, 0), Bytes({1}), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kLast, 20), Bytes({2}), m) == aa::MessageAssembler::kComplete);

    // A new FIRST replaces an unfinished message.
    CHECK(a.add(header(6, aa::frame::kFirst, 10, 1), Bytes({1}), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kFirst, 10, 1), Bytes({2}), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kLast, 10), Bytes({3}), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes({2, 3}));

    // Bounded per message: a whole frame leaves pending fragments intact, and an overflowing continuation
    // drops its message and skips the rest.
    const Bytes big(aa::frame::kMaxMessage - 10, 0);
    CHECK(a.add(header(6, aa::frame::kFirst, 1, 9), big, m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kMiddle, 1), Bytes(10, 0), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kWhole, 1), Bytes(1, 7), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes(1, 7));
    CHECK(a.add(header(6, aa::frame::kMiddle, 1), Bytes(1, 0), m) == aa::MessageAssembler::kOverflow);
    CHECK(a.add(header(6, aa::frame::kMiddle, 1), Bytes(1, 0), m) == aa::MessageAssembler::kRestOfLost);
    CHECK(a.add(header(6, aa::frame::kLast, 1), Bytes(1, 0), m) == aa::MessageAssembler::kRestOfLost);
    CHECK(a.add(header(6, aa::frame::kFirst, 1, 2), Bytes({11}), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kLast, 1), Bytes({12}), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes({11, 12}));
    // Past the bound at its LAST: nothing of that message is left to skip.
    CHECK(a.add(header(6, aa::frame::kFirst, 1, 9), big, m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kLast, 1), Bytes(11, 0), m) == aa::MessageAssembler::kOverflow);
    CHECK(a.add(header(6, aa::frame::kLast, 1), Bytes(1, 0), m) == aa::MessageAssembler::kNoFirst);

    CHECK(a.add(header(6, aa::frame::kFirst, 1, 9), big, m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kFirst, 11, 12), Bytes(11, 4), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kLast, 1), Bytes({5}), m) == aa::MessageAssembler::kComplete);
    Bytes expected(11, 4);
    expected.push_back(5);
    CHECK(m == expected);

    // A whole frame larger than the limit is rejected without dropping fragments already in flight.
    CHECK(a.add(header(6, aa::frame::kFirst, 1, 2), Bytes({1}), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kWhole, 1), Bytes(aa::frame::kMaxMessage + 1, 0), m) ==
          aa::MessageAssembler::kOverflow);
    CHECK(a.add(header(6, aa::frame::kLast, 1), Bytes({2}), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes({1, 2}));

    // An oversized replacement FIRST loses its own message, not just the preceding fragments.
    CHECK(a.add(header(6, aa::frame::kFirst, 1, 2), Bytes({1}), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kFirst, 1, 2), Bytes(aa::frame::kMaxMessage + 1, 0), m) ==
          aa::MessageAssembler::kOverflow);
    CHECK(a.add(header(6, aa::frame::kMiddle, 1), Bytes({2}), m) == aa::MessageAssembler::kRestOfLost);
    CHECK(a.add(header(6, aa::frame::kLast, 1), Bytes({3}), m) == aa::MessageAssembler::kRestOfLost);
    CHECK(a.add(header(6, aa::frame::kLast, 1), Bytes({4}), m) == aa::MessageAssembler::kNoFirst);
    a.clear();
}

// A fragment that does not decrypt loses its message: the rest of it is skipped up to its LAST, whole frames meanwhile
// pass, and the next message on the channel is rebuilt as usual.
void lostMessages() {
    aa::MessageAssembler a;
    Bytes m;
    // Lost at a MIDDLE.
    CHECK(a.add(header(6, aa::frame::kFirst, 30, 2), Bytes({1}), m) == aa::MessageAssembler::kPending);
    a.lose(header(6, aa::frame::kMiddle, 30));
    CHECK(a.add(header(6, aa::frame::kMiddle, 30), Bytes({2}), m) == aa::MessageAssembler::kRestOfLost);
    CHECK(a.add(header(6, aa::frame::kWhole, 30), Bytes({9}), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes({9}));
    CHECK(a.add(header(6, aa::frame::kLast, 30), Bytes({3}), m) == aa::MessageAssembler::kRestOfLost);
    CHECK(a.add(header(6, aa::frame::kFirst, 30, 2), Bytes({11}), m) == aa::MessageAssembler::kPending);
    CHECK(a.add(header(6, aa::frame::kLast, 30), Bytes({12}), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes({11, 12}));

    // Lost at its FIRST, and later fragments that do not decrypt either: one message skipped, the other channel's
    // message untouched.
    a.lose(header(6, aa::frame::kFirst, 30, 6));
    CHECK(a.add(header(8, aa::frame::kFirst, 30, 2), Bytes({7}), m) == aa::MessageAssembler::kPending);
    a.lose(header(6, aa::frame::kMiddle, 30));
    CHECK(a.add(header(6, aa::frame::kMiddle, 30), Bytes({2}), m) == aa::MessageAssembler::kRestOfLost);
    CHECK(a.add(header(6, aa::frame::kLast, 30), Bytes({3}), m) == aa::MessageAssembler::kRestOfLost);
    CHECK(a.add(header(8, aa::frame::kLast, 30), Bytes({8}), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes({7, 8}));

    // Lost at its LAST: nothing left to skip, the next continuation is an orphan again.
    CHECK(a.add(header(6, aa::frame::kFirst, 30, 2), Bytes({1}), m) == aa::MessageAssembler::kPending);
    a.lose(header(6, aa::frame::kLast, 30));
    CHECK(a.add(header(6, aa::frame::kLast, 30), Bytes({2}), m) == aa::MessageAssembler::kNoFirst);

    // An orphan MIDDLE that does not decrypt: the rest of its message is skipped too.
    a.lose(header(6, aa::frame::kMiddle, 30));
    CHECK(a.add(header(6, aa::frame::kLast, 30), Bytes({3}), m) == aa::MessageAssembler::kRestOfLost);

    // A whole frame that does not decrypt is no fragment of the message being assembled.
    CHECK(a.add(header(6, aa::frame::kFirst, 30, 2), Bytes({1}), m) == aa::MessageAssembler::kPending);
    a.lose(header(6, aa::frame::kWhole, 30));
    CHECK(a.add(header(6, aa::frame::kLast, 30), Bytes({2}), m) == aa::MessageAssembler::kComplete);
    CHECK(m == Bytes({1, 2}));
}

}  // namespace

int main() {
    headers();
    reassembly();
    puts("PASS frames: header both ways, 64 KiB limit, reassembly per channel, orphan fragments, size checks");
    lostMessages();
    puts("PASS frames: a message lost to an unreadable fragment is skipped up to its LAST, whole frames pass");
    return 0;
}
