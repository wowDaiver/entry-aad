// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdio.h>

#include <limits>

#include "menu_visibility.hpp"
int main() {
    telemetry::MenuVisibility menu;
    assert(!menu.active(0));
    assert(menu.change(10, true, 1));
    assert(menu.active(1));
    assert(menu.change(10, false, 2));
    assert(menu.active(2.05));  // native parent hides first
    assert(menu.change(11, true, 2.06));
    assert(menu.active(2.5));
    assert(menu.change(10, true, 3));  // opposite ordering on Back
    assert(menu.change(11, false, 3.01));
    assert(menu.active(3.5));
    assert(menu.change(10, false, 4));
    assert(menu.active(4.09));
    assert(!menu.active(4.11));
    assert(!menu.change(580, true, 5));  // outside this PluginApp
    assert(!menu.change(10, true, 3));
    assert(!menu.change(10, true, std::numeric_limits<double>::quiet_NaN()));
    assert(menu.change(14, true, 5));
    assert(menu.active(5));
    menu.clear();
    assert(!menu.active(5));
    assert(menu.change(15, true, 6));
    assert(menu.active(6));
    assert(!menu.change(18, true, 6));
    assert(menu.change(15, false, 7));
    assert(menu.active(7.05));
    assert(!menu.active(7.11));
    assert(menu.change(16, true, 8));
    assert(menu.change(16, false, 9));
    assert(menu.change(17, true, 9.05));
    assert(menu.active(12));
    assert(menu.change(17, false, 13));
    assert(menu.change(16, true, 13.05));
    assert(menu.active(14));
    assert(menu.change(16, false, 15));
    assert(!menu.active(15.11));
    puts("menu: parent/child transitions, Back order, external exit and invalid state checks passed");
}
