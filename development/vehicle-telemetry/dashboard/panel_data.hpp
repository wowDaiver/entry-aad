// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>

#include "../source_mapping.hpp"
#include "../telemetry_model.hpp"
namespace telemetry {
using std::isfinite;
// Extras are emulator fixtures only. Native brake/IGR readings take precedence.
// Never derive brake/recuperation from P, speed, voltage or current sign.
struct PanelSample {
    double value, received_at;
    Status status;
    PanelSample() : value(0), received_at(0), status(Unavailable) {}
    PanelSample(double v, Status s, double t) : value(v), received_at(t), status(s) {}
    bool has_value() const { return status == Valid || status == Stale; }
};
struct PanelExtras {
    PanelSample torque, brake, recuperation;
};
inline PanelSample panel_sample(PanelSample s, double now) {
    if (!isfinite(now) || !isfinite(s.value) || !isfinite(s.received_at) || now < s.received_at || !s.has_value() ||
        now - s.received_at >= 60) {
        return PanelSample();
    }
    if (now - s.received_at >= 5) {
        s.status = Stale;
    }
    return s;
}
inline PanelSample panel_metric(const Model& m, unsigned k, double now) {
    const Reading& r = m.reading(k);
    Status st = m.status(k, now);
    if (!r.present || !isfinite(r.value) || !isfinite(now) || now < r.received_at || now - r.received_at >= 60) {
        return PanelSample();
    }
    return PanelSample(r.value, st, r.received_at);
}
inline PanelSample panel_power(const PanelSample& torque, const PanelSample& rpm) {
    if (torque.status != Valid || rpm.status != Valid || !isfinite(torque.value) || !isfinite(rpm.value) ||
        rpm.value < 0 || fabs(torque.received_at - rpm.received_at) > 2) {
        return PanelSample();
    }
    double hp = torque.value * rpm.value / 9549.2966 / 0.73549875;
    return isfinite(hp) ? PanelSample(hp, Valid, std::min(torque.received_at, rpm.received_at)) : PanelSample();
}
struct PanelData {
    enum Field {
        Temp = PanelTemp,
        Oil = PanelOil,
        Rev = PanelRev,
        Velocity = PanelVelocity,
        Load = PanelLoad,
        Torque = PanelTorque,
        Power = PanelPower,
        Volt = PanelVolt,
        Current = PanelCurrent,
        Soc = PanelSoc,
        Atf = PanelAtf,
        Mode = PanelMode,
        Gear = PanelGear,
        Brake = PanelBrake,
        Regen = PanelRegen,
        Count = PanelCount
    };
    PanelSample rows[Count];
    PanelData(const Model& m, const PanelExtras& extras, double now) {
        for (unsigned i = 0; i < MetricCount; ++i) {
            const SourceBinding& b = *source_binding(i);
            if (b.panel_slot >= 0) {
                rows[b.panel_slot] = panel_metric(m, i, now);
            }
        }
        rows[Soc] = panel_metric(m, m.preferred_soc_metric(now), now);
        // DA2E deliberately has no S code. D + a fresh M/S gate is S;
        // actual manual mode M always takes precedence over gate position.
        const PanelSample gate = panel_metric(m, GearLever, now);
        if (rows[Mode].status == Valid && rows[Mode].value == 3 && gate.status == Valid && gate.value == 7 &&
            fabs(rows[Mode].received_at - gate.received_at) <= 2) {
            rows[Mode].value = 5;
            rows[Mode].received_at = std::min(rows[Mode].received_at, gate.received_at);
        }
        if (!rows[Torque].has_value()) {
            rows[Torque] = panel_sample(extras.torque, now);
        }
        if (!rows[Brake].has_value()) {
            rows[Brake] = panel_sample(extras.brake, now);
        }
        if (!rows[Regen].has_value()) {
            rows[Regen] = panel_sample(extras.recuperation, now);
        }
        // A past active interval is not evidence of activity now.
        if (m.reading(RecuperationActive).present && m.status(RecuperationActive, now) != Valid) {
            rows[Regen] = PanelSample();
        }
        if (rows[Brake].has_value() && rows[Brake].value != 0 && rows[Brake].value != 1) {
            rows[Brake] = PanelSample();
        }
        if (rows[Regen].has_value() && rows[Regen].value != 0 && rows[Regen].value != 1) {
            rows[Regen] = PanelSample();
        }
        rows[Power] = panel_power(rows[Torque], rows[Rev]);
    }
};
}  // namespace telemetry
