// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telemetry_model.hpp"
int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    char* tail = 0;
    double now = strtod(argv[1], &tail);
    if (!tail || *tail) {
        return 2;
    }
    telemetry::Model model(0);
    char line[1300];
    while (fgets(line, sizeof(line), stdin)) {
        char* end = 0;
        unsigned long index = strtoul(line, &end, 10);
        if (end == line || index >= telemetry::kProperties) {
            return 2;
        }
        char* start = end;
        double time = strtod(start, &end);
        if (end == start) {
            return 2;
        }
        while (*end == ' ') {
            ++end;
        }
        model.update(static_cast<unsigned>(index), end, time);
    }
    printf("{");
    for (unsigned i = 0; i < telemetry::MetricCount; ++i) {
        const telemetry::Reading& row = model.reading(i);
        printf("%s\"%s\":{\"state\":\"%s\",\"value\":", i ? "," : "", telemetry::metric_name(i),
               telemetry::status_name(model.status(i, now)));
        if (row.present) {
            printf("%.17g", row.value);
        } else {
            printf("null");
        }
        printf(",\"unit\":\"%s\",\"received_at\":", telemetry::unit_name(row.unit));
        if (row.present) {
            printf("%.17g", row.received_at);
        } else {
            printf("null");
        }
        printf(",\"measured_at\":null,\"meaning_verified_on_146\":false}");
    }
    puts("}");
    return 0;
}
