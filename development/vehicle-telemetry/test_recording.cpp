// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include <string>

#include "dashboard/panel_data.hpp"
#include "dashboard/recording.hpp"
using namespace telemetry;
static bool load(const std::string& body, Model& model, double now) {
    FILE* file = tmpfile();
    assert(file);
    assert(fwrite(body.data(), 1, body.size(), file) == body.size());
    rewind(file);
    bool ok = load_recording(file, model, now);
    fclose(file);
    return ok;
}
int main() {
    Model model(100);
    assert(load("I 12 0f1ff0\nD 12 43c0 7fa3\nD 12 5866 39e1\nI 18 0f21d0\nD 18 da2e 0000\n", model, 100));
    PanelData panel(model, PanelExtras(), 101);
    assert(fabs(panel.rows[PanelData::Current].value + 4.6) < 1e-9);
    assert(panel.rows[PanelData::Mode].value == 0 && panel.rows[PanelData::Mode].status == Valid);
    assert(!panel.rows[PanelData::Brake].has_value() && !panel.rows[PanelData::Regen].has_value());
    assert(model.reading(BatteryCurrent).received_at == 100);
    assert(PanelData(model, PanelExtras(), 105).rows[PanelData::Current].status == Stale);
    assert(!PanelData(model, PanelExtras(), 161).rows[PanelData::Current].has_value());
    // Loading an incomplete/corrupt file is transactional; no half-new data is displayed.
    assert(!load("I 12 0f1ff0\nD 12 5866 3a00\nbroken\n", model, 102));
    assert(fabs(model.reading(Voltage).value - 14.817) < 1e-9 && model.reading(Voltage).received_at == 100);
    assert(!load("I 12 0f1ff0\nD 12 5866 3a00", model, 102));
    assert(!load_recording(0, model, 102));
    std::string flood;
    for (unsigned i = 0; i < 257; ++i) {
        flood += "I 12 0f1ff0\n";
    }
    assert(!load(flood, model, 102));
    // Foreign identity invalidates an already accepted value and blocks more records.
    assert(load("I 12 0f1ff0\nD 12 5866 39e1\nI 12 0f1234\nD 12 5866 3a00\n", model, 102));
    assert(model.status(Voltage, 102) == Stale && fabs(model.reading(Voltage).value - 14.817) < 1e-9);
    // A new recording never inherits identity, values, or demo extras from a prior capture.
    assert(load("D 12 5866 39e1\n", model, 103));
    assert(!model.reading(Voltage).present);
    assert(load("I 12 0f1ff0\nD 12 586f c8\n", model, 104));
    assert(!model.reading(OilPressure).present);
    puts(
        "recording: bounded transactional load, identity isolation, archived timestamps, expiry and no demo fallback "
        "passed");
}
