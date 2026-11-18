// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace telemetry {
struct DiagnosticSource {
    unsigned did, size;
    double interval;
};
static const unsigned MaxDiagnosticSources = 17;
// Only records answered by this vehicle on 2026-11-17. Profiles are checked
// before every connection's first measurement; no sessions or discovery.
inline const DiagnosticSource* diagnostic_sources(unsigned ecu, bool expanded, unsigned& count) {
    static const DiagnosticSource temperature[] = {{0x6000, 1, 5}};
    static const DiagnosticSource dde[] = {{0x42b9, 2, 1}, {0x43c0, 2, 1}, {0x5868, 1, 2},  {0x5866, 2, 2},
                                           {0x0104, 1, 1}, {0x43c2, 1, 5}, {0x48a5, 2, .5}, {0x40c0, 16, 1}};
    static const DiagnosticSource egs[] = {{0x6000, 1, 2}, {0xda2e, 2, 1},  {0xda22, 2, 2},
                                           {0xda2a, 4, 1}, {0x6009, 1, 2},  {0x600a, 1, 2},
                                           {0x600c, 1, 2}, {0xda27, 2, .5}, {0xda31, 2, .5}};
    static const DiagnosticSource all[] = {
        {0x42b9, 2, 1}, {0x43c0, 2, 1},  {0x5868, 1, 2},  {0x5866, 2, 2},  {0x0104, 1, 1}, {0x6000, 1, 2},
        {0xda2e, 2, 1}, {0xda22, 2, 2},  {0xda2a, 4, 1},  {0x6009, 1, 2},  {0x600a, 1, 2}, {0x600c, 1, 2},
        {0x43c2, 1, 5}, {0x48a5, 2, .5}, {0xda27, 2, .5}, {0x40c0, 16, 1}, {0xda31, 2, .5}};
    if (ecu == 0 && expanded) {
        count = 17;
        return all;
    }
    if (ecu == 0x12 && expanded) {
        count = 8;
        return dde;
    }
    if (ecu == 0x18) {
        count = expanded ? 9 : 1;
        return expanded ? egs : temperature;
    }
    count = 0;
    return 0;
}
inline unsigned diagnostic_ecu(unsigned did) {
    switch (did) {
        case 0x42b9:
        case 0x43c0:
        case 0x5868:
        case 0x5866:
        case 0x0104:
        case 0x43c2:
        case 0x48a5:
        case 0x40c0:
            return 0x12;
        default:
            return 0x18;
    }
}
inline bool diagnostic_identity(unsigned ecu, const unsigned char* data, unsigned size) {
    if (!data || size != 3) {
        return false;
    }
    return data[0] == 15 &&
           ((ecu == 0x12 && data[1] == 0x1f && data[2] == 0xf0) || (ecu == 0x18 && data[1] == 0x21 && data[2] == 0xd0));
}
enum DiagnosticFailure { NoDiagnosticFailure, SocketFailure, ResponseTimeout, ProtocolFailure, IdentityFailure };
struct DiagnosticReply {
    unsigned ecu, did, size;
    unsigned char data[16];
    bool valid;
    unsigned failure;
};
}  // namespace telemetry
