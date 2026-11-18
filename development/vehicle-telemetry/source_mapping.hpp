// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "telemetry_model.hpp"

namespace telemetry {
enum SourceKind { UnknownSource, CdsSource, UdsSource, ObdSource };
enum PanelSlot {
    PanelTemp,
    PanelOil,
    PanelRev,
    PanelVelocity,
    PanelLoad,
    PanelTorque,
    PanelPower,
    PanelVolt,
    PanelCurrent,
    PanelSoc,
    PanelAtf,
    PanelMode,
    PanelGear,
    PanelBrake,
    PanelRegen,
    PanelCount
};
struct SourceBinding {
    Metric metric;
    SourceKind kind;
    unsigned ecu, key;  // CDS property index, UDS DID or the internal OBD 01/PID key.
    const char* path;
    unsigned hmi_model, hmi_state;
    int panel_slot;  // -1: detailed text page only, not the main instrument panel.
};
inline const SourceBinding* source_binding(unsigned metric) {
    // Order is Metric, not query order. Model IDs remain compatible with the XML.
    static const SourceBinding bindings[] = {
        {EngineTemperature, CdsSource, 0, 0, "temperature.engine", 100, 11, PanelTemp},
        {OilTemperature, CdsSource, 0, 0, "temperature.oil", 101, 11, PanelOil},
        {Rpm, CdsSource, 0, 1, "RPMSpeed", 102, 14, PanelRev},
        {Speed, CdsSource, 0, 2, "speedActual", 103, 14, PanelVelocity},
        {AverageConsumption, CdsSource, 0, 3, "averageConsumption.averageConsumption1", 104, 12, -1},
        {BatterySoc, CdsSource, 0, 4, "battery", 105, 13, PanelSoc},
        {Boost, UnknownSource, 0, 0, "", 106, 12, -1},
        {IntakeTemperature, UnknownSource, 0, 0, "", 107, 12, -1},
        {ExhaustTemperature, UnknownSource, 0, 0, "", 108, 11, -1},
        {Voltage, UdsSource, 0x12, 0x5866, "", 109, 13, PanelVolt},
        {FuelPressure, UnknownSource, 0, 0, "", 110, 12, -1},
        {RailPressure, UnknownSource, 0, 0, "", 111, 12, -1},
        {EngineLoad, ObdSource, 0x12, 0x0104, "", 112, 11, PanelLoad},
        {GearboxTemperature, UdsSource, 0x18, 0x6000, "", 113, 16, PanelAtf},
        {OilPressure, UdsSource, 0x12, 0x42b9, "", 130, 11, -1},
        {BatteryCurrent, UdsSource, 0x12, 0x43c0, "", 131, 13, PanelCurrent},
        {BatteryTemperature, UdsSource, 0x12, 0x5868, "", 132, 13, -1},
        {GearSelector, UdsSource, 0x18, 0xda2e, "byte0:TAB_ISTGANG", 133, 16, PanelGear},
        {GearMode, UdsSource, 0x18, 0xda2e, "byte1:TAB_FAHRSTUFE", 134, 16, PanelMode},
        {ConverterLock, UdsSource, 0x18, 0xda22, "", 135, 16, -1},
        {ConverterRpm, UdsSource, 0x18, 0xda2a, "bytes0..1", 136, 17, -1},
        {OutputShaftRpm, UdsSource, 0x18, 0xda2a, "bytes2..3", 137, 17, -1},
        {TurbineRpm, UdsSource, 0x18, 0x6009, "", 138, 17, -1},
        {EgsEngineRpm, UdsSource, 0x18, 0x600a, "", 139, 17, -1},
        {TransmissionGearCode, UdsSource, 0x18, 0x600c, "", 140, 17, -1},
        // No extra text models: torque is a dashboard field; SoC and lever
        // feed the existing SoC and mode displays through explicit selection.
        {EngineTorque, UdsSource, 0x12, 0x48a5, "ActMod_trq", 0, 15, PanelTorque},
        {BatterySocDde, UdsSource, 0x12, 0x43c2, "BMWfepm_pct_StorSoc_sb", 0, 0, -1},
        {GearLever, UdsSource, 0x18, 0xda27, "byte0:WAEHLHEBELPOSITION;byte1:bus-errors", 0, 0, -1},
        {ParkingBrake, UdsSource, 0x18, 0xda31, "TAB_FESTSTELLBREMSE", 0, 15, PanelBrake},
        {RecuperatedCharge, UdsSource, 0x12, 0x40c0, "BE32[0]/10;IGR recuperation charge since battery change", 0, 0,
         -1},
        {RecuperationActive, UdsSource, 0x12, 0x40c0,
         "derived:positive counter delta;0<dt<=3s;not instantaneous generator mode", 0, 15, PanelRegen}};
    static_assert(sizeof(bindings) / sizeof(bindings[0]) == MetricCount, "every metric needs an explicit binding");
    return metric < MetricCount ? bindings + metric : 0;
}
inline const char* panel_slot_name(unsigned slot) {
    static const char* names[] = {"temperature", "oil", "rpm", "speed", "load", "torque", "power",       "voltage",
                                  "current",     "soc", "atf", "mode",  "gear", "brake",  "recuperation"};
    static_assert(sizeof(names) / sizeof(names[0]) == PanelCount, "panel names must be complete");
    return slot < PanelCount ? names[slot] : "";
}
inline const char* source_kind_name(SourceKind kind) {
    const char* names[] = {"unidentified", "cds", "uds", "obd01"};
    return names[kind];
}
}  // namespace telemetry
