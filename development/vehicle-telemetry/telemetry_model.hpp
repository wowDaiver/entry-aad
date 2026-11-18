// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "probe.hpp"

namespace telemetry {
enum Metric {
    EngineTemperature,
    OilTemperature,
    Rpm,
    Speed,
    AverageConsumption,
    BatterySoc,
    Boost,
    IntakeTemperature,
    ExhaustTemperature,
    Voltage,
    FuelPressure,
    RailPressure,
    EngineLoad,
    GearboxTemperature,
    OilPressure,
    BatteryCurrent,
    BatteryTemperature,
    GearSelector,
    GearMode,
    ConverterLock,
    ConverterRpm,
    OutputShaftRpm,
    TurbineRpm,
    EgsEngineRpm,
    TransmissionGearCode,
    EngineTorque,
    BatterySocDde,
    GearLever,
    ParkingBrake,
    RecuperatedCharge,
    RecuperationActive,
    MetricCount
};
enum Status { Loading, Valid, Stale, Unavailable };
enum Unit {
    NoUnit,
    Celsius,
    Revolutions,
    Kmh,
    Litres100km,
    MpgUk,
    MpgUs,
    KmL,
    Percent,
    Bars,
    Amperes,
    Volts,
    SelectorPosition,
    DriveMode,
    LockState,
    GearCode,
    NewtonMetres,
    LeverCode,
    BooleanValue,
    AmpereSeconds
};
enum Error {
    NoError,
    Waiting,
    SourceUnknown,
    BadJson,
    BadValue,
    MissingField,
    SourceError,
    UnknownUnit,
    NotAvailable,
    InvalidClock,
    Stopped
};
struct Reading {
    double value, received_at;
    Unit unit;
    Error error;
    bool present;
};
class Model {
public:
    explicit Model(double opened_at);
    bool update(unsigned property, const char* json, double now);
    void unavailable(unsigned property, double now);
    void enable_gearbox(double now);
    void gearbox(double value, bool valid, double now);
    // Decode a complete positive UDS data record (without 62/DID). No I/O.
    // Candidate catalog layouts: callers must verify ECU identity/layout before
    // using this for live polling. No new requests are enabled by this method.
    bool diagnostic(unsigned ecu, unsigned did, const unsigned char*, unsigned size, double now);
    void enable_diagnostics(unsigned ecu, double now);
    void diagnostic_unavailable(unsigned ecu, unsigned did, double now);
    void stop(double now);
    Status status(unsigned metric, double now) const;
    const Reading& reading(unsigned metric) const;
    unsigned preferred_soc_metric(double now) const;
    double last_time() const { return last_time_; }

private:
    void fail(unsigned property, Error error);
    bool clock(double now);
    Reading rows_[MetricCount];
    double opened_at_, last_time_;
    bool stopped_;
};
const char* status_name(Status);
const char* metric_name(unsigned);
const char* unit_name(Unit);
unsigned metric_model(unsigned);
const char16_t* enum_value(Unit, double);
}  // namespace telemetry
