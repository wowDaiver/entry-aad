"""Offline model for bounded CDS replies; source meanings still need validation on 146."""

import json
import math
from dataclasses import dataclass

METRICS = {
    "engine_temperature": ("engine.temperature", ("temperature", "engine"), "°C", -48, 206, 30),
    "oil_temperature": ("engine.temperature", ("temperature", "oil"), "°C", -48, 206, 30),
    "rpm": ("engine.RPMSpeed", ("RPMSpeed",), "rpm", 0, 16383, 2),
    "speed": ("driving.speedActual", ("speedActual",), "km/h", 0, 1023, 2),
    "average_consumption": (
        "driving.averageConsumption",
        ("averageConsumption", "averageConsumption1"),
        None,
        0,
        409.2,
        30,
    ),
    "battery_soc": ("sensors.battery", ("battery",), "%", 0, 100, 30),
}
UNRESOLVED = (
    "boost",
    "intake_temperature",
    "exhaust_temperature",
    "voltage",
    "fuel_pressure",
    "rail_pressure",
    "engine_load",
    "gearbox_temperature",
    "oil_pressure",
    "battery_current",
    "battery_temperature",
    "gear_selector",
    "gear_mode",
    "converter_lock",
    "converter_rpm",
    "output_shaft_rpm",
    "turbine_rpm",
    "egs_engine_rpm",
    "transmission_gear_code",
    "engine_torque",
    "battery_soc_dde",
    "gear_lever",
    "parking_brake",
    "recuperated_charge",
    "recuperation_active",
)
CONSUMPTION_UNITS = {1: "l/100km", 2: "mpg_uk", 3: "mpg_us", 4: "km/l"}


def numeric(value):
    if type(value) not in (int, float):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:
        return False


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def parse(raw):
    if type(raw) is not str or len(raw.encode("utf-8")) > 1024:
        raise ValueError("payload exceeds bound or is not text")

    def invalid_constant(_):
        raise ValueError("non-JSON numeric constant")

    obj = json.loads(raw, object_pairs_hook=unique_object, parse_constant=invalid_constant)
    if type(obj) is not dict:
        raise ValueError("expected root object")
    if "error" in obj:
        raise ValueError("CDS error response")
    return obj


@dataclass
class Sample:
    value: object = None
    unit: object = None
    received_at: object = None
    error: object = None


class Model:
    def __init__(self, opened_at):
        if not numeric(opened_at) or opened_at < 0:
            raise ValueError("invalid monotonic start time")
        self.opened_at = self.last_time = opened_at
        self.samples = {name: Sample() for name in METRICS}

    def update(self, property_name, raw, received_at):
        if not numeric(received_at) or received_at < self.last_time:
            raise ValueError("invalid or decreasing monotonic receive time")
        keys = [key for key, spec in METRICS.items() if spec[0] == property_name]
        if not keys:
            raise ValueError("property not in probe allowlist")
        self.last_time = received_at
        try:
            obj = parse(raw)
        except (ValueError, RecursionError) as exc:
            for key in keys:
                self.samples[key].error = str(exc)
            return
        for key in keys:
            _, path, unit, low, high, _ = METRICS[key]
            sample = self.samples[key]
            try:
                value = obj
                for field in path:
                    if type(value) is not dict:
                        raise ValueError("wrong object type")
                    value = value[field]
                if not numeric(value) or not low <= value <= high:
                    raise ValueError("invalid value or sentinel")
                if key in ("rpm", "battery_soc") and value != int(value):
                    raise ValueError("expected integral value")
                if key == "average_consumption":
                    code = obj["averageConsumption"]["unit"]
                    if type(code) is not int or code not in CONSUMPTION_UNITS:
                        raise ValueError("unknown consumption unit")
                    unit = CONSUMPTION_UNITS[code]
                sample.value, sample.unit = value, unit
                sample.received_at, sample.error = received_at, None
            except (KeyError, ValueError) as exc:
                # Retain last good value and its ORIGINAL receive timestamp.
                sample.error = str(exc)

    def snapshot(self, now):
        if not numeric(now) or now < self.last_time:
            raise ValueError("invalid snapshot time")
        result = {}
        for key, spec in METRICS.items():
            sample = self.samples[key]
            if sample.received_at is None:
                state = "unavailable" if sample.error or now - self.opened_at >= 5 else "loading"
            else:
                state = "stale" if sample.error or now - sample.received_at >= spec[5] else "valid"
            result[key] = dict(
                state=state,
                value=sample.value,
                unit=sample.unit,
                received_at=sample.received_at,
                measured_at=None,
                error=sample.error,
                source=spec[0],
                meaning_verified_on_146=False,
            )
        for key in UNRESOLVED:
            result[key] = dict(state="unavailable", value=None, error="source_not_identified")
        return result
