#include <assert.h>
#include <stdio.h>
#include <time.h>

#include <limits>
#include <string>

#include "demo_fixture.hpp"
#include "renderer.hpp"
using namespace telemetry;
static void save_frame(const std::string& path, const std::vector<unsigned char>& png) {
    FILE* f = fopen(path.c_str(), "wb");
    assert(f);
    assert(fwrite(png.data(), 1, png.size(), f) == png.size());
    fclose(f);
}
int main(int argc, char** argv) {
    assert(argc == 3);
    DashboardRenderer d(DashboardWidth, DashboardHeight, argv[1]);
    Model m(0);
    PanelExtras extras;
    const std::string prefix = std::string(argv[2]).substr(0, std::string(argv[2]).find_last_of('.'));
    panel_demo(m, extras, "live", 1, 0);
    PanelData live(m, extras, 1);
    assert(live.rows[PanelData::Torque].value == 165 && fabs(live.rows[PanelData::Power].value - 42.281) < .02);
    assert(live.rows[PanelData::Current].value > 8.39 && live.rows[PanelData::Current].value < 8.41);
    // The displayed derived value must disappear on stale or skewed input.
    PanelSample freshTorque(165, Valid, 1), freshRpm(1800, Valid, 4);
    assert(!panel_power(freshTorque, freshRpm).has_value());
    PanelExtras skew = extras;
    skew.torque.received_at = std::numeric_limits<double>::quiet_NaN();
    assert(!PanelData(m, skew, 1).rows[PanelData::Torque].has_value());
    PanelData production(m, PanelExtras(), 1);
    assert(!production.rows[PanelData::Torque].has_value());
    assert(!production.rows[PanelData::Brake].has_value() && !production.rows[PanelData::Regen].has_value());
    // Positive charging current must not invent recuperation or a parking brake.
    const std::vector<unsigned char>& png = d.render(m, 1, true, extras);
    save_frame(argv[2], png);
    clock_t begin = clock();
    for (int i = 0; i < 20; ++i) {
        panel_demo(m, extras, "live", 2 + i, i + 1);
        d.render(m, 2 + i, true, extras);
    }
    printf("PNG=%zu bytes render=%.2f ms/frame\n", png.size(), 1000. * (clock() - begin) / CLOCKS_PER_SEC / 20);
    unsigned frames = d.frames();
    d.render(m, 21, true, extras);
    assert(!d.changed() && d.frames() == frames);
    panel_demo(m, extras, "stale", 22, 0);
    PanelData stale(m, extras, 22);
    assert(stale.rows[PanelData::Torque].status == Stale && !stale.rows[PanelData::Power].has_value());
    save_frame(prefix + "-stale.png", d.render(m, 22, true, extras));
    assert(!PanelData(m, extras, 90).rows[PanelData::Torque].has_value());
    d.render(m, 90, true, extras);
    panel_demo(m, extras, "regen", 91, 0);
    PanelData regen(m, extras, 91);
    assert(regen.rows[PanelData::Regen].value == 1 && !regen.rows[PanelData::Power].has_value());
    save_frame(prefix + "-regen.png", d.render(m, 91, true, extras));
    panel_demo(m, extras, "parked", 92, 0);
    assert(PanelData(m, extras, 92).rows[PanelData::Velocity].value == 0);
    save_frame(prefix + "-parked.png", d.render(m, 92, true, extras));
    panel_demo(m, extras, "empty", 93, 0);
    PanelData empty(m, extras, 93);
    for (unsigned i = 0; i < PanelData::Count; ++i) {
        assert(!empty.rows[i].has_value());
    }
    save_frame(prefix + "-empty.png", d.render(m, 93, true, extras));
    panel_demo(m, extras, "live", 94, 0);
    assert(PanelData(m, extras, 94).rows[PanelData::Power].status == Valid);
    d.render(m, 94, true, extras);
    bool rejected = false;
    try {
        DashboardRenderer bad(4096, 320, argv[1]);
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    rejected = false;
    try {
        DashboardRenderer bad(960, 320, "/missing-dashboard-font");
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    puts(
        "Panel: all fields, independent unknown statuses, power validity/skew, zero speed, missing, stale expiry, "
        "recovery and PNG cache passed");
}
