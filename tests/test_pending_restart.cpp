// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The restart asked by the menu's USB checkbox: started once, only when nothing of the service is left, dropped by a
// start or a stop by hand, and given up when the stop never ends.
#include <stdio.h>
#include <stdlib.h>

#include "../hmiapp/pending_restart.hpp"

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                            \
        }                                                       \
    } while (0)

using evo::app::PendingRestart;

int main() {
    {  // Nothing asked: nothing happens, busy or not.
        PendingRestart r;
        CHECK(!r.asked() && r.turn(10, false) == PendingRestart::kNothing &&
              r.turn(11, true) == PendingRestart::kNothing);
    }
    {  // Asked while Android Auto runs: waits for the stop, starts once, then nothing more.
        PendingRestart r;
        r.ask(100);
        CHECK(r.asked());
        for (long t = 100; t < 112; ++t) {
            CHECK(r.turn(t, true) == PendingRestart::kNothing);
        }
        CHECK(r.turn(112, false) == PendingRestart::kStart && !r.asked());
        CHECK(r.turn(113, false) == PendingRestart::kNothing && r.turn(114, true) == PendingRestart::kNothing);
    }
    {  // The service was already gone at the first poll: started at once.
        PendingRestart r;
        r.ask(5);
        CHECK(r.turn(5, false) == PendingRestart::kStart);
    }
    {  // A stop that never ends: given up once, and a service ending later is not started behind the user's back.
        PendingRestart r;
        r.ask(1000);
        CHECK(r.turn(1000 + evo::app::kRestartGiveUpS - 1, true) == PendingRestart::kNothing);
        CHECK(r.turn(1000 + evo::app::kRestartGiveUpS, true) == PendingRestart::kGiveUp && !r.asked());
        CHECK(r.turn(1000 + evo::app::kRestartGiveUpS + 1, false) == PendingRestart::kNothing);
    }
    {  // Stop or Start by hand in the meantime: the restart is dropped.
        PendingRestart r;
        r.ask(50);
        CHECK(r.turn(51, true) == PendingRestart::kNothing);
        r.cancel();
        CHECK(!r.asked() && r.turn(60, false) == PendingRestart::kNothing);
    }
    {  // Asked again before the first restart happened (the box ticked, then unticked): one restart, counted from the
       // last change.
        PendingRestart r;
        r.ask(10);
        r.ask(40);
        CHECK(r.turn(10 + evo::app::kRestartGiveUpS, true) == PendingRestart::kNothing);
        CHECK(r.turn(60, false) == PendingRestart::kStart && r.turn(61, false) == PendingRestart::kNothing);
    }
    puts("PASS pending restart: waits for the stop, starts once, dropped by hand, given up after the limit");
    return 0;
}
