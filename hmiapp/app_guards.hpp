// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// What keeps the app from harming iDrive, and the automatic start they protect. Files in /var/local/data/AA:
//   app.disable   present: the app stays off (iDrive starts without it)
//   app-guard     crash guard: one line per load of the app; 3 loads that did not reach 2 minutes of uptime (an HMI
//                 crash loop) and the app refuses to start. Removed once the HMI has been up 2 minutes with it.
//   autostart     present: Android Auto starts by itself 30 s after iDrive
//   usb           present: Android Auto over a USB cable instead of wireless (docs/USB.md)
//   bootguard     the automatic starts of the last 15 minutes; at 3, the automatic start is skipped (Start in the menu
//                 still works). Removed by a manual start and once the HMI has been up 2 minutes.
#ifndef ENTRY_AAD_HMIAPP_APP_GUARDS_HPP
#define ENTRY_AAD_HMIAPP_APP_GUARDS_HPP

#include <time.h>

namespace evo {
namespace app {

// When iDrive creates the app; each logs why it keeps the app off.
bool appDisabled();       // app.disable is present
bool crashGuardAllows();  // counts this load; false after 3 loads without 2 minutes of uptime

// Once per status poll, initTime being when onInit ran.
void resetGuardsWhenStable(time_t initTime);  // 2 minutes up: not a crash loop
void serviceAutostart(time_t initTime);       // the automatic start, once, 30 s after onInit

void forgetBootGuard();  // a start by hand
bool autostartOn();
void toggleAutostart();  // the menu's checkbox
// The menu's other checkbox. Ticking creates the empty file (every USB port); unticking removes it, with the
// port name someone may have written in it by hand.
bool usbCableOn();
void toggleUsbCable();

}  // namespace app
}  // namespace evo

#endif
