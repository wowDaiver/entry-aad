// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <string>

#include "recording.hpp"
#include "renderer.hpp"
using namespace telemetry;
namespace {
std::string utf8(const char16_t* text) {
    std::string out;
    for (; *text; ++text) {
        unsigned c = *text;
        if (c < 128) {
            out += char(c);
        } else if (c < 2048) {
            out += char(192 | (c >> 6));
            out += char(128 | (c & 63));
        } else {
            out += char(224 | (c >> 12));
            out += char(128 | ((c >> 6) & 63));
            out += char(128 | (c & 63));
        }
    }
    return out;
}
void reading_json(const Reading& r, Status status) {
    printf("\"status\":\"%s\",\"value\":", status_name(status));
    if (r.present) {
        printf("%.17g", r.value);
    } else {
        printf("null");
    }
    printf(",\"unit\":\"%s\",\"received_at_replay\":", unit_name(r.unit));
    if (r.present) {
        printf("%.17g", r.received_at);
    } else {
        printf("null");
    }
    printf(",\"measured_at\":null,\"enum_text\":");
    if (r.present && (r.unit == SelectorPosition || r.unit == DriveMode || r.unit == LockState || r.unit == GearCode)) {
        printf("\"%s\"", utf8(enum_value(r.unit, r.value)).c_str());
    } else {
        printf("null");
    }
}
}  // namespace
int main(int argc, char** argv) {
    if (argc != 4) {
        return 2;
    }
    char* end = 0;
    double age = strtod(argv[3], &end);
    if (end == argv[3] || *end || !std::isfinite(age) || age < 0) {
        return 2;
    }
    Model model(0);
    SourceInput input(model);
    char line[1400];
    unsigned event = 0;
    printf("{\"kind\":\"offline-block-interface-replay\",\"clock\":\"replay_seconds\",\"now\":%.17g,\"events\":[", age);
    while (fgets(line, sizeof(line), stdin)) {
        SourceEvent e = {};
        if (++event > 256 || !apply_source_event(input, line, 0, e)) {
            return 2;
        }
        printf("%s{\"type\":\"%c\",\"ecu\":%u,\"key\":%u,\"accepted\":%s}", event > 1 ? "," : "", e.type, e.ecu, e.key,
               e.accepted ? "true" : "false");
    }
    printf("],\"metrics\":[");
    for (unsigned i = 0; i < MetricCount; ++i) {
        const SourceBinding& b = *source_binding(i);
        printf(
            "%s{\"name\":\"%s\",\"source\":\"%s\",\"ecu\":%u,\"key\":%u,\"path\":\"%s\",\"hmi_model\":%u,\"hmi_state\":"
            "%u,\"panel_slot\":",
            i ? "," : "", metric_name(i), source_kind_name(b.kind), b.ecu, b.key, b.path, b.hmi_model, b.hmi_state);
        if (b.panel_slot >= 0) {
            printf("\"%s\"", panel_slot_name(b.panel_slot));
        } else {
            printf("null");
        }
        printf(",\"cds_property\":");
        if (b.kind == CdsSource) {
            printf("\"%s\"", properties[b.key].name);
        } else {
            printf("null");
        }
        printf(",");
        reading_json(model.reading(i), model.status(i, age));
        printf("}");
    }
    PanelData panel(model, PanelExtras(), age);
    printf("],\"panel\":{");
    for (unsigned i = 0; i < PanelCount; ++i) {
        const PanelSample& s = panel.rows[i];
        printf("%s\"%s\":{\"status\":\"%s\",\"value\":", i ? "," : "", panel_slot_name(i), status_name(s.status));
        if (s.has_value()) {
            printf("%.17g", s.value);
        } else {
            printf("null");
        }
        printf("}");
    }
    printf("}}\n");
    DashboardRenderer renderer(DashboardWidth, DashboardHeight, argv[1]);
    const std::vector<unsigned char>& png = renderer.render(model, age, false, PanelExtras(), true);
    FILE* f = fopen(argv[2], "wb");
    if (!f) {
        return 3;
    }
    bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    if (fclose(f)) {
        ok = false;
    }
    return ok ? 0 : 3;
}
