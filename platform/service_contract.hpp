// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// What the iDrive app, the supervisor (aa-supervisor) and the service (entry-aad) share: file paths and the control
// port.
//
// The installation procedure (docs/INSTALLATION.md, docs/MAINTENANCE.md) names the same paths: change both together.
#ifndef ENTRY_AAD_PLATFORM_SERVICE_CONTRACT_HPP
#define ENTRY_AAD_PLATFORM_SERVICE_CONTRACT_HPP

#include <stdint.h>

namespace evo {
namespace contract {

// ---- programs, on the system partition ----------------------------------------------------------------------------
constexpr char kSupervisor[] = "/opt/aa/aa-supervisor";
constexpr char kService[] = "/opt/aa/entry-aad";

// ---- settings and logs, on the data partition (kept across restarts) ----------------------------------------------
constexpr char kDataDir[] = "/var/local/data/AA";
constexpr char kAutostart[] = "/var/local/data/AA/autostart";         // present: Android Auto starts with iDrive
constexpr char kEntryDirect[] = "/var/local/data/AA/entry-direct";    // present: the iDrive row opens Android Auto
constexpr char kMaintenance[] = "/var/local/data/AA/maintenance";     // present: the app and the service keep logs
constexpr char kKombiOff[] = "/var/local/data/AA/kombi-off";          // present: nothing is sent to the cluster
constexpr char kKombiShown[] = "/var/local/data/AA/kombi-nav.shown";  // an arrow may still be on the cluster
constexpr char kPhoneGps[] = "/var/local/data/AA/gps-telephone";      // present: the phone keeps its own GPS
constexpr char kVehicle[] = "/var/local/data/AA/vehicle";             // model=... and year=... told to the phone
// Present: Android Auto over a USB cable instead of wireless. Empty, or the one USB port to use ("1-1").
constexpr char kUsb[] = "/var/local/data/AA/usb";
constexpr char kAppLog[] = "/var/local/data/AA/app.log";
constexpr char kAppLogOld[] = "/var/local/data/AA/app.old.log";
constexpr char kAppDisable[] = "/var/local/data/AA/app.disable";  // present: the app stays off (set by hand)
constexpr char kAppGuard[] = "/var/local/data/AA/app-guard";   // crash guard: 3 short iDrive runs and the app stays off
constexpr char kBootGuard[] = "/var/local/data/AA/bootguard";  // automatic starts within 15 minutes

// ---- run time, in /tmp (memory, gone at the next start) -----------------------------------------------------------
constexpr char kServiceDir[] = "/tmp/entry-aa-service";
constexpr char kServiceStop[] = "/tmp/entry-aa-service/STOP";  // created by the app: the supervisor stops
constexpr char kServiceLog[] = "/tmp/entry-aa-service.log";
constexpr char kStateFile[] = "/tmp/entry-aad-state";             // "state pid overlay", written by the service
constexpr char kAppKeyFile[] = "/tmp/entry-aad-appkey";           // NAV/MEDIA/COM opened an Android Auto app
constexpr char kDayNightFile[] = "/tmp/entry-aad-daynight";       // day or night, written by the app
constexpr char kWirelessLock[] = "/tmp/entry-aad-wireless.lock";  // one wireless start at a time

// ---- control messages from the app to the service: UDP on 127.0.0.1 only ------------------------------------------
constexpr uint16_t kControlPort = 52790;

}  // namespace contract
}  // namespace evo

#endif
