// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// hmiapp/popup_peek.hpp: when Android Auto steps aside for an iDrive popup.
#include <assert.h>
#include <stdio.h>

#include "../hmiapp/popup_peek.hpp"

int main() {
    {  // a popup: one start, no repeat, one end
        PopupPeek p;
        assert(p.step(false, 100, 0) == PopupPeek::kNone && !p.active());
        assert(p.step(true, 101, 0) == PopupPeek::kStart && p.active());
        assert(p.step(true, 102, 0) == PopupPeek::kNone);
        assert(p.step(false, 103, 0) == PopupPeek::kEnd && !p.active());
        assert(p.step(false, 104, 0) == PopupPeek::kNone);
    }
    {  // the driving mode popup keeps its own shorter peek, for the whole of its span
        PopupPeek p;
        assert(p.step(true, 200, 198) == PopupPeek::kNone && !p.active());
        assert(p.step(true, 207, 198) == PopupPeek::kNone && !p.active());  // still open past the 6 s: not taken again
        assert(p.step(false, 208, 198) == PopupPeek::kNone);
        assert(p.step(true, 220, 198) == PopupPeek::kStart);  // a later popup is a real one
    }
    {  // a long popup does not take the picture again once its time is over; a new span does
        PopupPeek p;
        assert(p.step(true, 300, 0) == PopupPeek::kStart);
        for (long t = 301; t < 370; ++t) {
            assert(p.step(true, t, 0) == PopupPeek::kNone);
        }
        assert(p.step(false, 370, 0) == PopupPeek::kEnd);
        assert(p.step(true, 400, 0) == PopupPeek::kStart);
    }
    {  // the driving mode popup: only its setting read off, with no coding forcing it, means none (#28)
        assert(!driveModePopupExpected(0, 0));
        assert(driveModePopupExpected(1, 0) && driveModePopupExpected(1, 1) && driveModePopupExpected(0, 1));
        assert(driveModePopupExpected(-1, 0) && driveModePopupExpected(-1, -1) && driveModePopupExpected(0, -1));
        assert(driveModePopupExpected(1, -1) && driveModePopupExpected(-1, 1));
    }
    puts(
        "PASS iDrive popups: one step aside per popup, driving mode display left out, no repeat, driving mode popup "
        "turned off in iDrive");
    return 0;
}
