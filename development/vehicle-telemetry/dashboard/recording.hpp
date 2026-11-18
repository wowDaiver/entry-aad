// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdio.h>
#include <string.h>

#include "../source_input.hpp"

namespace telemetry {
struct SourceEvent {
    char type;
    unsigned ecu, key;
    bool accepted;
};
inline bool recording_hex(const char* text, unsigned char* bytes, unsigned& size) {
    size_t n = strlen(text);
    if (!n || n % 2 || n > 32) {
        return false;
    }
    size = n / 2;
    for (unsigned i = 0; i < size; ++i) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = text[i * 2 + j];
            unsigned d = c >= '0' && c <= '9'   ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                : 16;
            if (d > 15) {
                return false;
            }
            v = v * 16 + d;
        }
        bytes[i] = v;
    }
    return true;
}
inline bool apply_source_event(SourceInput& input, char* line, double now, SourceEvent& event) {
    // A full newline-terminated line is mandatory, so truncation cannot become data.
    if (!strchr(line, '\n')) {
        return false;
    }
    line[strcspn(line, "\r\n")] = 0;
    event = {line[0], 0, 0, false};
    unsigned size = 0;
    unsigned char bytes[16] = {};
    char data[40] = {};
    int used = 0;
    if (line[0] == 'I') {
        if (sscanf(line, "I %x %39s%n", &event.ecu, data, &used) != 2 || line[used] ||
            !recording_hex(data, bytes, size)) {
            return false;
        }
        event.accepted = input.identity(event.ecu, bytes, size);
    } else if (line[0] == 'D') {
        if (sscanf(line, "D %x %x %39s%n", &event.ecu, &event.key, data, &used) != 3 || line[used] ||
            !recording_hex(data, bytes, size)) {
            return false;
        }
        event.accepted = input.record(event.ecu, event.key, bytes, size, now);
    } else if (line[0] == 'N') {
        if (sscanf(line, "N %x %x%n", &event.ecu, &event.key, &used) != 2 || line[used]) {
            return false;
        }
        event.accepted = input.unavailable(event.ecu, event.key, now);
    } else if (line[0] == 'C') {
        if (sscanf(line, "C %u %n", &event.key, &used) != 1 || used <= 0 || event.key >= kProperties) {
            return false;
        }
        event.accepted = input.cds(event.key, line + used, now);
    } else {
        return false;
    }
    return true;
}
inline bool load_recording(FILE* file, Model& model, double now) {
    if (!file) {
        return false;
    }
    Model candidate(now);
    SourceInput input(candidate);
    char line[1400];
    unsigned count = 0;
    while (fgets(line, sizeof(line), file)) {
        SourceEvent event = {};
        if (++count > 256 || !apply_source_event(input, line, now, event)) {
            return false;
        }
    }
    if (!count || ferror(file)) {
        return false;
    }
    model = candidate;
    return true;  // Malformed files never partially overwrite the model.
}
}  // namespace telemetry
