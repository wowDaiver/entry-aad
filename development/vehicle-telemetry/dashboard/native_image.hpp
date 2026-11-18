// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../native_cds.hpp"
// Exact146 member extents, checked against the archived native constructors.
// Same boundary in the isolated emulator and exact-profile physical candidate.
namespace bmw {
class BufferStream {
public:
    BufferStream();
    virtual ~BufferStream();

private:
    unsigned fields_[5] __attribute__((unused));
};
class Image {
public:
    Image(const unsigned char*, unsigned);

private:
    BasicString<char> path_;
    BufferStream stream_;
    bool loaded_;
    Image(const Image&);
    Image& operator=(const Image&);
};
#ifndef TELEMETRY_HOST_ABI_STUB
static_assert(sizeof(BufferStream) == 24, "native146 BufferStream extent");
static_assert(sizeof(Image) == 32, "native146 Image extent and tail padding");
#endif
}  // namespace bmw
