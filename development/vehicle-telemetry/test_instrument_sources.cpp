// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdio.h>

#include <string>

#include "dashboard/panel_data.hpp"
using namespace telemetry;
int main() {
    Model m(0);
    const unsigned char soc[] = {0xd6}, torque[] = {0x5d, 0x19};
    assert(m.diagnostic(0x12, 0x43c2, soc, 1, 1));
    assert(m.diagnostic(0x12, 0x48a5, torque, 2, 1));
    assert(m.update(4, "{\"battery\":255}", 1.1));
    assert(m.update(1, "{\"RPMSpeed\":1500}", 1.1));
    PanelData p(m, PanelExtras(), 1.1);
    assert(p.rows[PanelData::Soc].value == 87 && p.rows[PanelData::Soc].status == Valid);
    assert(fabs(p.rows[PanelData::Torque].value - 227.51191578) < 1e-6);
    assert(fabs(p.rows[PanelData::Power].value - (227.51191578 * 1500 * 2 * 3.141592653589793 / 60 / 735.49875)) <
           .001);
    assert(m.update(4, "{\"battery\":61}", 1.2));
    assert(m.preferred_soc_metric(1.2) == BatterySocDde);
    m.diagnostic_unavailable(0x12, 0x43c2, 1.3);
    assert(m.preferred_soc_metric(1.3) == BatterySoc);
    assert(PanelData(m, PanelExtras(), 1.3).rows[PanelData::Soc].value == 61);
    const unsigned char no_soc[] = {0};
    assert(m.diagnostic(0x12, 0x43c2, no_soc, 1, 1.4));
    assert(m.reading(BatterySocDde).received_at == 1 && m.reading(BatterySocDde).value == 87);
    const unsigned char invalid_torque[] = {255, 255};
    assert(m.diagnostic(0x12, 0x48a5, invalid_torque, 2, 1.5));
    assert(m.reading(EngineTorque).received_at == 1);
    assert(!PanelData(m, PanelExtras(), 1.5).rows[PanelData::Power].has_value());
    Model gear(0);
    unsigned char mode[] = {9, 3}, lever[] = {7, 0};
    assert(gear.diagnostic(0x18, 0xda2e, mode, 2, 1));
    assert(gear.diagnostic(0x18, 0xda27, lever, 2, 1.1));
    assert(PanelData(gear, PanelExtras(), 1.1).rows[PanelData::Mode].value == 5);
    assert(std::u16string(enum_value(DriveMode, 5)) == u"S");
    mode[1] = 4;
    assert(gear.diagnostic(0x18, 0xda2e, mode, 2, 1.2));
    assert(PanelData(gear, PanelExtras(), 1.2).rows[PanelData::Mode].value == 4);  // M wins over M/S gate.
    mode[1] = 3;
    assert(gear.diagnostic(0x18, 0xda2e, mode, 2, 1.3));
    lever[1] = 1;
    assert(gear.diagnostic(0x18, 0xda27, lever, 2, 1.4));
    assert(PanelData(gear, PanelExtras(), 1.4).rows[PanelData::Mode].value == 3);  // bus error cannot invent S.
    assert(!gear.diagnostic(0x18, 0xda27, lever, 1, 1.5));
    Model skew(0);
    mode[1] = 3;
    lever[1] = 0;
    assert(skew.diagnostic(0x18, 0xda2e, mode, 2, 0));
    assert(skew.diagnostic(0x18, 0xda27, lever, 2, 3));
    assert(PanelData(skew, PanelExtras(), 3).rows[PanelData::Mode].value == 3);
    assert(skew.diagnostic(0x18, 0xda2e, mode, 2, 4));
    assert(PanelData(skew, PanelExtras(), 4).rows[PanelData::Mode].value == 5);
    Model brakes(0);
    unsigned char brake[] = {0, 1};
    assert(brakes.diagnostic(0x18, 0xda31, brake, 2, 1));
    assert(PanelData(brakes, PanelExtras(), 1).rows[PanelData::Brake].value == 1);
    brake[1] = 0;
    assert(brakes.diagnostic(0x18, 0xda31, brake, 2, 2));
    assert(PanelData(brakes, PanelExtras(), 2).rows[PanelData::Brake].value == 0);
    brake[0] = brake[1] = 255;
    assert(brakes.diagnostic(0x18, 0xda31, brake, 2, 3));
    assert(brakes.status(ParkingBrake, 3) == Stale && brakes.reading(ParkingBrake).received_at == 2);
    Model energy(0);
    unsigned char counters[16] = {3, 0x68, 0x99, 0xfc, 8, 8, 0x51, 0xef, 0, 0x12, 0x74, 0xf0, 0, 0x0c, 0xca, 0x54};
    assert(energy.diagnostic(0x12, 0x40c0, counters, 16, 1));
    assert(!PanelData(energy, PanelExtras(), 1).rows[PanelData::Regen].has_value());  // requires two observations
    assert(fabs(energy.reading(RecuperatedCharge).value - 5718681.2) < 1e-6);
    counters[7]++;
    assert(energy.diagnostic(0x12, 0x40c0, counters, 16, 2));  // ordinary charging grows, IGR does not
    assert(PanelData(energy, PanelExtras(), 2).rows[PanelData::Regen].status == Valid);
    assert(PanelData(energy, PanelExtras(), 2).rows[PanelData::Regen].value == 0);
    counters[3]++;
    assert(energy.diagnostic(0x12, 0x40c0, counters, 16, 3));
    assert(PanelData(energy, PanelExtras(), 3).rows[PanelData::Regen].value == 1);
    assert(energy.diagnostic(0x12, 0x40c0, counters, 16, 3));
    assert(energy.reading(RecuperationActive).received_at == 3);  // same clock does not fabricate another interval
    assert(!PanelData(energy, PanelExtras(), 6)
                .rows[PanelData::Regen]
                .has_value());  // never retain an expired active indicator
    counters[0] = 0;
    assert(energy.diagnostic(0x12, 0x40c0, counters, 16, 6));
    assert(!PanelData(energy, PanelExtras(), 6).rows[PanelData::Regen].has_value());  // reset/wrap
    assert(energy.diagnostic(0x12, 0x40c0, counters, 16, 7));
    assert(PanelData(energy, PanelExtras(), 7).rows[PanelData::Regen].value == 0);
    energy.diagnostic_unavailable(0x12, 0x40c0, 7.1);
    assert(!PanelData(energy, PanelExtras(), 7.1).rows[PanelData::Regen].has_value());
    counters[3]++;
    assert(energy.diagnostic(0x12, 0x40c0, counters, 16, 8));
    assert(!PanelData(energy, PanelExtras(), 8)
                .rows[PanelData::Regen]
                .has_value());  // recovery needs another complete interval
    counters[3]++;
    assert(energy.diagnostic(0x12, 0x40c0, counters, 16, 9));
    assert(PanelData(energy, PanelExtras(), 9).rows[PanelData::Regen].value == 1);
    counters[3]++;
    assert(energy.diagnostic(0x12, 0x40c0, counters, 16, 13));
    assert(
        !PanelData(energy, PanelExtras(), 13).rows[PanelData::Regen].has_value());  // long gap is not current activity
    for (unsigned i = 0; i < 4; ++i) {
        counters[i] = 255;
    }
    assert(energy.diagnostic(0x12, 0x40c0, counters, 16, 14));
    assert(!PanelData(energy, PanelExtras(), 14).rows[PanelData::Regen].has_value());
    puts(
        "instruments: recorded SoC/torque, independent CDS fallback, power validity and D/S/M gate/skew/error checks "
        "passed");
}
