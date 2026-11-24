// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Protobuf wire format, both ways: building the messages sent to the phone, and reading what it sends.
//
// Reading never trusts a length: each one is compared with what is left before the position moves, so a hostile value
// can neither wrap the position (size_t is 32 bits on the car) nor send the reader backwards into an endless loop.
// Tested in tests/test_pb.cpp, on the PC and on the car's ARM.
#ifndef ENTRY_AAD_CORE_PB_HPP
#define ENTRY_AAD_CORE_PB_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aa {
namespace pb {

enum WireType : unsigned { kVarint = 0, kFixed64 = 1, kBytes = 2, kFixed32 = 5 };

// ---- writing ----------------------------------------------------------------------------------------------------

// Base-128 varint, lowest seven bits first.
inline void appendVarint(std::vector<uint8_t>& out, uint64_t value) {
    for (; value >= 0x80; value >>= 7) {
        out.push_back(static_cast<uint8_t>(value | 0x80));
    }
    out.push_back(static_cast<uint8_t>(value));
}

inline void appendKey(std::vector<uint8_t>& out, uint32_t field, WireType type) {
    appendVarint(out, (static_cast<uint64_t>(field) << 3) | type);
}

// A varint field. A negative int32 or int64 goes in as static_cast<uint64_t>(value): ten bytes, as protobuf does.
inline void appendUint(std::vector<uint8_t>& out, uint32_t field, uint64_t value) {
    appendKey(out, field, kVarint);
    appendVarint(out, value);
}

inline void appendBytes(std::vector<uint8_t>& out, uint32_t field, const uint8_t* data, size_t size) {
    appendKey(out, field, kBytes);
    appendVarint(out, size);
    out.insert(out.end(), data, data + size);
}

inline void appendString(std::vector<uint8_t>& out, uint32_t field, const std::string& text) {
    appendBytes(out, field, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

inline void appendMessage(std::vector<uint8_t>& out, uint32_t field, const std::vector<uint8_t>& message) {
    appendBytes(out, field, message.data(), message.size());
}

// ---- reading ----------------------------------------------------------------------------------------------------

// One varint at data[pos]; false when it runs past size or past ten bytes.
inline bool readVarint(const uint8_t* data, size_t size, size_t& pos, uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64 && pos < size; shift += 7) {
        const uint8_t byte = data[pos++];
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            return true;
        }
    }
    return false;
}

// One field of a message, as FieldReader gives it.
struct Field {
    uint32_t number;
    unsigned type;        // WireType; any other value makes the message malformed
    uint64_t value;       // varint, fixed32 or fixed64 (little-endian) value
    const uint8_t* data;  // length-delimited value
    size_t size;
    size_t begin;  // the whole field, key included, as data[begin, end) of the message
    size_t end;
};

// Walks the top level of a message, field by field. next() returns false at the end or on malformed input;
// malformed() tells the two apart.
class FieldReader {
public:
    FieldReader(const uint8_t* data, size_t size) : data_(data), size_(size), pos_(0), malformed_(false) {}

    bool next(Field& field) {
        if (pos_ >= size_ || malformed_) {
            return false;
        }
        field.begin = pos_;
        uint64_t key = 0;
        if (!readVarint(data_, size_, pos_, key) || (key >> 3) == 0 || (key >> 3) > 0x1fffffff) {
            return fail();
        }
        field.number = static_cast<uint32_t>(key >> 3);
        field.type = static_cast<unsigned>(key & 7);
        field.value = 0;
        field.data = nullptr;
        field.size = 0;
        if (field.type == kVarint) {
            if (!readVarint(data_, size_, pos_, field.value)) {
                return fail();
            }
        } else if (field.type == kFixed64 || field.type == kFixed32) {
            const size_t width = field.type == kFixed64 ? 8 : 4;
            if (width > size_ - pos_) {
                return fail();
            }
            for (size_t i = 0; i < width; ++i) {
                field.value |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
            }
            pos_ += width;
        } else if (field.type == kBytes) {
            uint64_t length = 0;
            if (!readVarint(data_, size_, pos_, length) || length > size_ - pos_) {
                return fail();
            }
            field.data = data_ + pos_;
            field.size = static_cast<size_t>(length);
            pos_ += field.size;
        } else {
            return fail();
        }
        field.end = pos_;
        return true;
    }

    bool malformed() const { return malformed_; }

private:
    bool fail() {
        malformed_ = true;
        return false;
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_;
    bool malformed_;
};

// Copies a message field by field into `out`. For each field, `edit(field, out)` may append what replaces it (nothing
// drops it) and return true, or return false to have the field copied as it was. False when the message is malformed:
// `out` then holds the fields before the fault.
template <typename Edit>
bool rewriteMessage(const uint8_t* data, size_t size, std::vector<uint8_t>& out, Edit edit) {
    FieldReader reader(data, size);
    Field field;
    while (reader.next(field)) {
        if (!edit(field, out)) {
            out.insert(out.end(), data + field.begin, data + field.end);
        }
    }
    return !reader.malformed();
}

inline bool isMessage(const Field& field, uint32_t number) {
    return field.type == kBytes && field.number == number;
}

// First varint field `number` at the top level of a message. False when absent, or when the message is malformed
// before it.
inline bool findVarint(const uint8_t* data, size_t size, uint32_t number, uint64_t& value) {
    FieldReader reader(data, size);
    Field field;
    while (reader.next(field)) {
        if (field.number == number && field.type == kVarint) {
            value = field.value;
            return true;
        }
    }
    return false;
}

}  // namespace pb
}  // namespace aa

#endif
