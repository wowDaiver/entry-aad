// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// What the volume pill shows: the knob's level on a change or after a key, another source's only as the call's volume
// during a call, and mute.
#include <stdio.h>
#include <stdlib.h>

#include "volume_state.hpp"

#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                             \
        }                                                        \
    } while (0)

namespace {

evo::VolumeLevel level(unsigned value, unsigned min, unsigned max) {
    evo::VolumeLevel l = {value, min, max};
    return l;
}

bool shows(const evo::VolumeState::Shown& s, unsigned value, unsigned min, unsigned max) {
    return s.show && s.level.value == value && s.level.min == min && s.level.max == max;
}

void atClock(uint32_t t) {
    evo::VolumeState v;
    CHECK(!v.known() && !v.muted());
    // The MINI F57 at connection: another source's level, again and again, while the knob's volume is 3.
    for (int i = 0; i < 10; ++i) {
        CHECK(!v.level(level(25, 25, 100), t + 300 * i).show);
    }
    CHECK(!v.known());
    // The first level of the knob's volume shows; the same one again, nothing having changed, does not.
    CHECK(shows(v.level(level(3, 0, 100), t + 4000), 3, 0, 100));
    CHECK(!v.level(level(3, 0, 100), t + 4300).show);
    // A change shows.
    CHECK(shows(v.level(level(5, 0, 100), t + 5000), 5, 0, 100));
    // A volume key: the next level shows even unchanged (the knob at its end stop), not one past the window.
    v.volumeKey(t + 6000);
    CHECK(shows(v.level(level(5, 0, 100), t + 6100), 5, 0, 100));
    CHECK(!v.level(level(5, 0, 100), t + 6000 + evo::VolumeState::kKeyWindowMs + 1).show);
    // The audio knob: muted, the speaker crossed out at the minimum; the level kept for later.
    CHECK(shows(v.audioButton(t + 7000), 0, 0, 100) && v.muted());
    CHECK(shows(v.level(level(5, 0, 100), t + 7100), 0, 0, 100));  // within the button's window: still muted
    // Pressed again: the sound back, at its level.
    CHECK(shows(v.audioButton(t + 8000), 5, 0, 100) && !v.muted());
    // A volume step ends a mute, as iDrive does.
    v.audioButton(t + 9000);
    CHECK(v.muted());
    v.volumeKey(t + 9500);
    CHECK(!v.muted() && shows(v.level(level(6, 0, 100), t + 9600), 6, 0, 100));
    // The button before any level: muted, nothing to show yet.
    evo::VolumeState fresh;
    CHECK(!fresh.audioButton(t).show && fresh.muted());
    CHECK(shows(fresh.level(level(8, 0, 40), t + 100), 0, 0, 40));
}

// A call on the MINI F56 of #88 (service log): the knob's volume at 33, then the call's volume between 25 and 100.
void duringCall(uint32_t t) {
    evo::VolumeState v;
    CHECK(shows(v.level(level(33, 0, 100), t), 33, 0, 100));
    // The call starts: its volume comes by itself and does not show; the knob and the wheel then move it.
    v.call(true);
    CHECK(!v.level(level(53, 25, 100), t + 250).show);
    v.volumeKey(t + 3500);
    CHECK(shows(v.level(level(55, 25, 100), t + 3500), 55, 25, 100));
    v.volumeKey(t + 3600);
    CHECK(shows(v.level(level(57, 25, 100), t + 3600), 57, 25, 100));
    // The same level again, no key: nothing; a change without a key shows.
    CHECK(!v.level(level(57, 25, 100), t + 3600 + evo::VolumeState::kKeyWindowMs + 1).show);
    CHECK(shows(v.level(level(53, 25, 100), t + 9000), 53, 25, 100));
    // The call ends: the call's volume never shows again, the knob's comes back as before.
    v.call(false);
    v.volumeKey(t + 10000);
    CHECK(!v.level(level(55, 25, 100), t + 10000).show);
    CHECK(shows(v.level(level(35, 0, 100), t + 10100), 35, 0, 100));
    // Next call: its first level, the same as the last one, still does not show by itself.
    v.call(true);
    CHECK(!v.level(level(55, 25, 100), t + 20000).show);
    v.volumeKey(t + 21000);
    CHECK(shows(v.level(level(57, 25, 100), t + 21000), 57, 25, 100));
}

}  // namespace

int main() {
    atClock(1000);
    atClock(0xFFFFF000u);  // across the 32-bit wrap of the millisecond clock
    atClock(0);            // a key at time 0 still counts
    duringCall(1000);
    duringCall(0xFFFFF000u);
    puts("PASS volume state: another source's level ignored, repeats held back, keys, mute by the audio knob");
    puts("PASS volume state: during a call, the call's volume shows after a key or on a change (#88)");
    return 0;
}
