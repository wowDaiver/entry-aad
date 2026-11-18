// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "telemetry_model.hpp"

namespace telemetry {
// Vehicle-proven layouts for ECU12 0F1FF0 / ECU18 0F21D0 (2026-11-17).
// Reader verifies the identity before calling this pure bounded decoder.
// DDE definitions: D83BX7C0 SG_FUNKTIONEN, archived source/provenance in research.
// Conservative reserved/physical guards below do not define alarm thresholds.
struct DiagnosticSample {
    unsigned metric;
    double value;
    Unit unit;
    bool valid;
};
struct DiagnosticRecord {
    DiagnosticSample samples[3];
    unsigned count;
    DiagnosticRecord() : count(0) {}
    void add(unsigned metric, double value, Unit unit, bool valid = true) {
        samples[count++] = {metric, value, unit, valid};
    }
};
inline unsigned word(const unsigned char* p) {
    return unsigned(p[0]) * 256 + p[1];
}
inline int signed_word(const unsigned char* p) {
    unsigned n = word(p);
    return n >= 32768 ? int(n) - 65536 : int(n);
}
inline unsigned counter32(const unsigned char* p) {
    return (unsigned(p[0]) << 24) | (unsigned(p[1]) << 16) | (unsigned(p[2]) << 8) | p[3];
}
inline bool decode_diagnostic(unsigned ecu, unsigned did, const unsigned char* p, unsigned size, DiagnosticRecord& r) {
    r.count = 0;
    if (!p) {
        return false;
    }
    if (ecu == 0x18 && did == 0xda31 && size == 2) {
        r.add(ParkingBrake, word(p), BooleanValue, word(p) <= 1);
        return true;
    }
    if (ecu == 0x12 && did == 0x40c0 && size == 16) {
        // RES_0X40C0_D: first BE32 counts 0.1 As of charge in IGR
        // recuperation since the battery change. Other three counters differ.
        r.add(RecuperatedCharge, counter32(p) * .1, AmpereSeconds, counter32(p) != 0xffffffffu);
        return true;
    }
    if (ecu == 0x12 && did == 0x43c2 && size == 1) {
        r.add(BatterySocDde, int(p[0]) - 127, Percent, p[0] >= 127 && p[0] <= 227);
        return true;
    }
    if (ecu == 0x12 && did == 0x48a5 && size == 2) {
        r.add(EngineTorque, word(p) * 0.11444266 - 2500., NewtonMetres, word(p) != 65535);
        return true;
    }
    if (ecu == 0x18 && did == 0xda27 && size == 2) {
        r.add(GearLever, p[0], LeverCode, p[0] <= 7 && p[1] == 0);
        return true;
    }
    if (ecu == 0x12 && did == 0x0104 && size == 1) {
        r.add(EngineLoad, p[0] * 100. / 255, Percent);
        return true;
    }
    if (ecu == 0x12 && did == 0x42b9 && size == 2) {
        r.add(OilPressure, word(p) / 1000., Bars, word(p) != 65535);
        return true;
    }
    if (ecu == 0x12 && did == 0x43c0 && size == 2) {
        r.add(BatteryCurrent, word(p) * 0.05 - 1638.35, Amperes, word(p) != 65535);
        return true;
    }
    if (ecu == 0x12 && did == 0x5868 && size == 1) {
        r.add(BatteryTemperature, int(p[0]) - 126, Celsius, p[0] >= 66 && p[0] <= 251);
        return true;
    }
    if (ecu == 0x12 && did == 0x5866 && size == 2) {
        r.add(Voltage, word(p) * 0.001, Volts, word(p) >= 6000 && word(p) <= 22400);
        return true;
    }
    if (ecu == 0x18 && did == 0xda2e && size == 2) {
        bool selector = p[0] <= 9 || p[0] == 16;
        // GSZFB1 TAB_ISTGANG: P,R,N,gears1..8; TAB_FAHRSTUFE: P,R,N,D,M.
        r.add(GearSelector, p[0], SelectorPosition, selector);
        r.add(GearMode, p[1], DriveMode, p[1] <= 4);
        return true;
    }
    if (ecu == 0x18 && did == 0xda22 && size == 2) {
        unsigned state = word(p);
        r.add(ConverterLock, state, LockState, state <= 4 || (state >= 8 && state <= 12));
        return true;
    }
    if (ecu == 0x18 && did == 0xda2a && size == 4) {
        // Signed values per catalog: output rotation may be negative in R.
        r.add(ConverterRpm, signed_word(p), Revolutions, word(p) != 32767 && word(p) != 32768);
        r.add(OutputShaftRpm, signed_word(p + 2), Revolutions, word(p + 2) != 32767 && word(p + 2) != 32768);
        return true;
    }
    if (ecu == 0x18 && did == 0x6000 && size == 1) {
        r.add(GearboxTemperature, int(p[0]) - 40, Celsius, p[0] <= 245);
        return true;
    }
    if (ecu == 0x18 && (did == 0x6009 || did == 0x600a) && size == 1) {
        r.add(did == 0x6009 ? TurbineRpm : EgsEngineRpm, p[0] * 32, Revolutions, p[0] != 255);
        return true;
    }
    if (ecu == 0x18 && did == 0x600c && size == 1) {
        // GSZFB1 UWB_TAB_600C: FF = reverse, FE = invalid.
        r.add(TransmissionGearCode, p[0], GearCode, (p[0] >= 1 && p[0] <= 8) || p[0] == 255);
        return true;
    }
    return false;
}
}  // namespace telemetry
