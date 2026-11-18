// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdio.h>
#include <string.h>

#include "panel_data.hpp"
namespace telemetry {
// Called only by the explicitly enabled isolated emulator and host preview.
inline void panel_demo(Model& m, PanelExtras& extras, const char* scenario, double now, double second) {
    if (!strcmp(scenario, "empty")) {
        m = Model(now);
        extras = PanelExtras();
        return;
    }
    if (!strcmp(scenario, "stale")) {
        for (unsigned i = 0; i < kProperties; ++i) {
            m.unavailable(i, now);
        }
        m.diagnostic_unavailable(0x12, 0, now);
        m.diagnostic_unavailable(0x18, 0, now);
        if (extras.torque.has_value()) {
            extras.torque.status = Stale;
        }
        if (extras.brake.has_value()) {
            extras.brake.status = Stale;
        }
        if (extras.recuperation.has_value()) {
            extras.recuperation.status = Stale;
        }
        return;
    }
    bool parked = !strcmp(scenario, "parked"), regen = !strcmp(scenario, "regen");
    double phase = second * .16;
    char temp[128], rpm[64];
    snprintf(temp, sizeof(temp), "{\"temperature\":{\"engine\":%.0f,\"oil\":%.0f}}", 92 + sin(phase) * 2,
             99 + sin(phase * .5) * 2);
    snprintf(rpm, sizeof(rpm), "{\"RPMSpeed\":%.0f}",
             floor((parked  ? 780
                    : regen ? 2200
                            : 1800) +
                   sin(phase * 2) * (parked ? 30 : 60)));
    m.update(0, temp, now);
    m.update(1, rpm, now);
    m.update(2, parked ? "{\"speedActual\":0}" : "{\"speedActual\":64}", now);
    m.update(4, "{\"battery\":78}", now);
    unsigned char voltage[] = {0x39, 0x6c}, current[] = {0x80, 0xa7}, load[] = {107};
    if (regen) {
        voltage[0] = 0x3a;
        voltage[1] = 0x34;
        current[0] = 0x82;
        current[1] = 0xf7;
        load[0] = 0;
    }
    if (parked) {
        load[0] = 46;
    }
    const unsigned char gear[] = {static_cast<unsigned char>(parked ? 0 : 6),
                                  static_cast<unsigned char>(parked ? 0 : 3)};
    const unsigned char atf[] = {114};
    m.diagnostic(0x12, 0x5866, voltage, 2, now);
    m.diagnostic(0x12, 0x43c0, current, 2, now);
    m.diagnostic(0x12, 0x0104, load, 1, now);
    m.diagnostic(0x18, 0xda2e, gear, 2, now);
    m.diagnostic(0x18, 0x6000, atf, 1, now);
    extras.torque = regen ? PanelSample() : PanelSample(parked ? 35 : 165, Valid, now);
    extras.brake = PanelSample(parked ? 1 : 0, Valid, now);
    extras.recuperation = PanelSample(regen ? 1 : 0, Valid, now);
}
}  // namespace telemetry
