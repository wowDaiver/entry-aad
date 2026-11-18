// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include <string>

#include "diagnostic_records.hpp"
#include "telemetry_model.hpp"
using namespace telemetry;
int main() {
    Model m(0);
    m.enable_diagnostics(0x12, 0);
    m.enable_diagnostics(0x18, 0);
    assert(m.status(OilPressure, 0) == Loading && m.status(TurbineRpm, 0) == Loading);
    unsigned char oil[] = {0x05, 0x07};              // Actually captured: 1287 hPa = 1.287 bar.
    assert(!m.diagnostic(0x12, 0x586f, oil, 2, 1));  // FEV is NOT oil on 0F1FF0.
    assert(!m.diagnostic(0x18, 0x42b9, oil, 2, 1));
    assert(!m.diagnostic(0x12, 0x42b9, oil, 1, 1));
    assert(!m.diagnostic(0x12, 0x42b9, 0, 2, 1));
    assert(m.diagnostic(0x12, 0x42b9, oil, 2, 1));
    assert(fabs(m.reading(OilPressure).value - 1.287) < 1e-9 && m.reading(OilPressure).unit == Bars);
    unsigned char zero[] = {0, 0};
    assert(m.diagnostic(0x12, 0x42b9, zero, 2, 2));
    assert(m.status(OilPressure, 2) == Valid && m.reading(OilPressure).value == 0);
    unsigned char battery[] = {0x7f, 0xa3}, voltage[] = {0x39, 0xe1}, temperature[] = {0x86};
    assert(!m.diagnostic(0x12, 0x402b, battery, 2, 3));
    assert(m.diagnostic(0x12, 0x43c0, battery, 2, 3));
    assert(m.diagnostic(0x12, 0x5866, voltage, 2, 3));
    assert(m.diagnostic(0x12, 0x5868, temperature, 1, 3));
    assert(fabs(m.reading(BatteryCurrent).value + 4.6) < 1e-9);
    assert(fabs(m.reading(Voltage).value - 14.817) < 1e-9);
    assert(m.reading(BatteryTemperature).value == 8);
    voltage[0] = voltage[1] = 255;
    assert(m.diagnostic(0x12, 0x5866, voltage, 2, 4));
    assert(m.status(Voltage, 4) == Stale && m.reading(Voltage).received_at == 3);
    assert(m.status(BatteryCurrent, 4) == Valid && m.reading(BatteryCurrent).received_at == 3);
    assert(!m.diagnostic(0x12, 0x43c0, battery, 1, 5));
    assert(m.reading(BatteryCurrent).received_at == 3);
    m.diagnostic_unavailable(0x12, 0x43c0, 5);
    assert(m.status(BatteryCurrent, 5) == Stale && m.reading(BatteryCurrent).received_at == 3);
    assert(m.status(BatteryTemperature, 5) == Valid);  // one DID failure is isolated
    unsigned char selector[] = {16, 4};
    assert(m.diagnostic(0x18, 0xda2e, selector, 2, 6));
    assert(std::u16string(enum_value(SelectorPosition, m.reading(GearSelector).value)) == u"8");
    assert(std::u16string(enum_value(DriveMode, m.reading(GearMode).value)) == u"M");
    selector[0] = 255;
    selector[1] = 3;
    assert(m.diagnostic(0x18, 0xda2e, selector, 2, 7));
    assert(m.status(GearSelector, 7) == Stale && m.reading(GearSelector).received_at == 6);
    assert(m.status(GearMode, 7) == Valid);
    unsigned char lock[] = {0, 8};
    assert(m.diagnostic(0x18, 0xda22, lock, 2, 8));
    assert(std::u16string(enum_value(LockState, 8)) == u"Открывается");
    lock[1] = 255;
    assert(m.diagnostic(0x18, 0xda22, lock, 2, 9));
    assert(m.status(ConverterLock, 9) == Stale && m.reading(ConverterLock).received_at == 8);
    unsigned char speeds[] = {0x03, 0x20, 0xff, 0x9c};
    assert(m.diagnostic(0x18, 0xda2a, speeds, 4, 10));
    assert(m.reading(ConverterRpm).value == 800 && m.reading(OutputShaftRpm).value == -100);
    assert(m.status(OutputShaftRpm, 15) == Stale);
    assert(!m.diagnostic(0x18, 0xda2a, speeds, 4, 9));
    unsigned char atf[] = {113};
    assert(m.diagnostic(0x18, 0x6000, atf, 1, 12));
    assert(m.reading(GearboxTemperature).value == 73);
    atf[0] = 255;
    assert(m.diagnostic(0x18, 0x6000, atf, 1, 13));
    assert(m.status(GearboxTemperature, 13) == Stale && m.reading(GearboxTemperature).received_at == 12);
    m.stop(14);
    assert(!m.diagnostic(0x12, 0x42b9, oil, 2, 15));
    assert(m.status(BatteryCurrent, 15) == Stale && m.reading(BatteryCurrent).received_at == 3);
    Model empty(0);
    empty.enable_diagnostics(0x12, 0);
    unsigned char missing[] = {255};
    assert(empty.diagnostic(0x12, 0x5868, missing, 1, 1));
    assert(empty.status(BatteryTemperature, 1) == Unavailable);
    Model e(0);
    e.enable_diagnostics(0x18, 0);
    unsigned char turbine[] = {0x17}, engine[] = {0x18}, gear[] = {1};
    assert(e.diagnostic(0x18, 0x6009, turbine, 1, 1));
    assert(e.reading(TurbineRpm).value == 736);
    assert(e.diagnostic(0x18, 0x600a, engine, 1, 1));
    assert(e.reading(EgsEngineRpm).value == 768);
    assert(e.diagnostic(0x18, 0x600c, gear, 1, 1));
    assert(e.reading(TransmissionGearCode).value == 1);
    gear[0] = 255;
    assert(e.diagnostic(0x18, 0x600c, gear, 1, 1.1));
    assert(e.status(TransmissionGearCode, 1.1) == Valid && std::u16string(enum_value(GearCode, 255)) == u"R");
    unsigned char reverse[] = {1, 1};
    assert(e.diagnostic(0x18, 0xda2e, reverse, 2, 1.2));
    assert(e.status(GearMode, 1.2) == Valid && std::u16string(enum_value(DriveMode, 1)) == u"R");
    unsigned char transition[] = {0, 12};
    assert(e.diagnostic(0x18, 0xda22, transition, 2, 1.3));
    assert(e.status(ConverterLock, 1.3) == Valid);
    unsigned char load[] = {0x85};
    assert(e.diagnostic(0x12, 0x0104, load, 1, 1.4));
    assert(fabs(e.reading(EngineLoad).value - 52.156862745) < 1e-6);
    load[0] = 255;
    assert(e.diagnostic(0x12, 0x0104, load, 1, 1.5));
    assert(e.reading(EngineLoad).value == 100);
    e.diagnostic_unavailable(0x18, 0, 2);
    assert(e.status(TurbineRpm, 2) == Stale);
    assert(std::u16string(enum_value(DriveMode, 0)) == u"P");
    for (unsigned i = 0; i < MetricCount; ++i) {
        if (!metric_model(i)) {
            continue;  // dashboard-only / auxiliary inputs have no text model
        }
        assert(metric_model(i) != 114 && metric_model(i) != 120);
        for (unsigned j = 0; j < i; ++j) {
            if (metric_model(j)) {
                assert(metric_model(i) != metric_model(j));
            }
        }
    }
    puts("diagnostics: ECU/length gates, independent nulls, signs, enums, timestamps and stopped sessions passed");
}
