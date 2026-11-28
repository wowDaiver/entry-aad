// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "diagnostic_schedule.hpp"

namespace telemetry {
// Owner146's native CPxyDiagnosis/CTesterAssistent route. Channel A and SA63
// are the only combination verified on this firmware; B and SA F5 do not reply.
static const char MostSocketPath[] = "/run/vehicle-telemetry/most.sock";
static const unsigned MostFunction = 0x50;
struct MostRead {
    unsigned ecu, did, size;
    bool obd;
};
enum MostAnswer { IgnoreMostAnswer, PendingMostAnswer, FailedMostAnswer, CompleteMostAnswer };

// The worker accepts only these flat read requests. The ExecuteDiagnosticService
// envelope is constructed here, never accepted from an arbitrary caller.
inline bool decode_local_read(const uint8_t* p, size_t n, MostRead& out) {
    if (!p || (n != 10 && n != 11) || p[0] || p[1] || p[2] || p[3] != n - 6 || p[4] || p[5] != 1 || p[6] != 0xf4 ||
        (p[7] != 0x12 && p[7] != 0x18)) {
        return false;
    }
    out.ecu = p[7];
    out.obd = n == 10;
    if (out.obd) {
        if (out.ecu != 0x12 || p[8] != 1 || p[9] != 4) {
            return false;
        }
        out.did = 0x0104;
        out.size = 1;
        return true;
    }
    if (p[8] != 0x22) {
        return false;
    }
    out.did = (unsigned(p[9]) << 8) | p[10];
    if (out.did == 0xf150) {
        out.size = 3;
        return true;
    }
    unsigned count = 0;
    const DiagnosticSource* sources = diagnostic_sources(out.ecu, true, count);
    for (unsigned i = 0; i < count; ++i) {
        if (sources[i].did == out.did) {
            out.size = sources[i].size;
            return true;
        }
    }
    return false;
}

inline size_t encode_most_read(const MostRead& r, uint8_t (&out)[12]) {
    const uint8_t head[] = {0x63, 0xf0, 0x31, 1, 0x0f, 0x0b, uint8_t(r.ecu), 0, uint8_t(r.obd ? 2 : 3)};
    for (unsigned i = 0; i < sizeof(head); ++i) {
        out[i] = head[i];
    }
    out[9] = r.obd ? 1 : 0x22;
    out[10] = r.obd ? 4 : uint8_t(r.did >> 8);
    out[11] = uint8_t(r.did);
    return r.obd ? 11 : 12;
}

// Ignore other iDrive clients, records, channels and addresses. A response must
// identify the outstanding ECU and DID, have its exact length and be complete.
inline MostAnswer decode_most_answer(const MostRead& r, unsigned block, unsigned instance, unsigned function,
                                     unsigned operation, const uint8_t* p, size_t n, uint8_t (&hsfz)[32],
                                     size_t& output_size) {
    output_size = 0;
    if (block != 6 || instance != 0xf0 || function != MostFunction || operation != 0xc || !p || n < 5 || p[0] != 0xf0 ||
        p[1] != 0x63) {
        return IgnoreMostAnswer;
    }
    if (n == 5 && p[2] == 0x7f && p[3] == 0x31) {
        return p[4] == 0x78 ? PendingMostAnswer : FailedMostAnswer;
    }
    if (n < 9 || p[2] != 0x71 || p[3] != 1 || p[4] != 0x0f || p[5] != 0x0b || p[6] != r.ecu) {
        return IgnoreMostAnswer;
    }
    const unsigned size = (unsigned(p[7]) << 8) | p[8];
    if (size > 23 || n != size + 9) {
        return FailedMostAnswer;
    }
    const uint8_t* uds = p + 9;
    const unsigned service = r.obd ? 1 : 0x22;
    if (size == 3 && uds[0] == 0x7f && uds[1] == service) {
        if (uds[2] == 0x78) {
            return PendingMostAnswer;
        }
    } else {
        const unsigned prefix = r.obd ? 2 : 3;
        if (size < prefix || uds[0] != (r.obd ? 0x41 : 0x62) || uds[1] != (r.obd ? 4 : r.did >> 8) ||
            (!r.obd && uds[2] != (r.did & 255))) {
            return IgnoreMostAnswer;
        }
        if (size != r.size + prefix) {
            return FailedMostAnswer;
        }
    }
    for (unsigned i = 0; i < 8; ++i) {
        hsfz[i] = 0;
    }
    hsfz[3] = uint8_t(size + 2);
    hsfz[5] = 1;
    hsfz[6] = uint8_t(r.ecu);
    hsfz[7] = 0xf4;
    for (unsigned i = 0; i < size; ++i) {
        hsfz[i + 8] = uds[i];
    }
    output_size = size + 8;
    return CompleteMostAnswer;
}
}  // namespace telemetry
