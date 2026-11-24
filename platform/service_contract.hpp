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
constexpr char kVehicle[] = "/var/local/data/AA/vehicle";             // model= year= side= fuel=, told to the phone
// An Android Auto picture may still be on the cluster of an F-series car (over MOST, kombi_most.hpp).
constexpr char kKombiMostShown[] = "/var/local/data/AA/kombi-most.shown";
// Present: send only the next maneuver's graph to the cluster.
constexpr char kKombiSingle[] = "/var/local/data/AA/kombi-single-maneuver";
// Present: Android Auto over a USB cable instead of wireless. Empty, or the one USB port to use ("1-1").
constexpr char kUsb[] = "/var/local/data/AA/usb";
constexpr char kAppLog[] = "/var/local/data/AA/app.log";
constexpr char kAppLogOld[] = "/var/local/data/AA/app.old.log";
// The service's log in the maintenance profile, kept across the car's sleep (kept_log.hpp).
constexpr char kServiceLogKept[] = "/var/local/data/AA/service.log";
constexpr char kServiceLogKeptOld[] = "/var/local/data/AA/service.old.log";
constexpr char kAppDisable[] = "/var/local/data/AA/app.disable";  // present: the app stays off (set by hand)
constexpr char kAppGuard[] = "/var/local/data/AA/app-guard";   // crash guard: 3 short iDrive runs and the app stays off
constexpr char kBootGuard[] = "/var/local/data/AA/bootguard";  // automatic starts within 15 minutes
// The update from a USB stick (update_install.hpp): what its last run found and did, and its journal while an update
// is being swapped in (the signed list and its signature).
constexpr char kUpdateReport[] = "/var/local/data/AA/update-report.txt";
constexpr char kUpdatePending[] = "/var/local/data/AA/update-pending";

// ---- run time, in /tmp (memory, gone at the next start) -----------------------------------------------------------
constexpr char kServiceDir[] = "/tmp/entry-aa-service";
constexpr char kServiceStop[] = "/tmp/entry-aa-service/STOP";  // created by the app: the supervisor stops
// A link to kServiceLogKept, so that what reads the log here still finds it; the log itself when the data partition
// refused it.
constexpr char kServiceLog[] = "/tmp/entry-aa-service.log";
constexpr char kStateFile[] = "/tmp/entry-aad-state";             // "state pid overlay", written by the service
constexpr char kAppKeyFile[] = "/tmp/entry-aad-appkey";           // NAV/MEDIA/COM/BACK kept Android Auto up
constexpr char kDayNightFile[] = "/tmp/entry-aad-daynight";       // day or night, written by the app
constexpr char kControllerFile[] = "/tmp/entry-aad-controller";   // the car's key coding, written by the app
constexpr char kBrandFile[] = "/tmp/entry-aad-brand";             // "BMW" or "MINI", as iDrive is coded
constexpr char kDriverSideFile[] = "/tmp/entry-aad-driver-side";  // "left" or "right", as the car is coded
// The car's series as iDrive is coded ("G20"), written by the app; the model told to the phone without one in vehicle.
constexpr char kSeriesFile[] = "/tmp/entry-aad-series";
constexpr char kWirelessLock[] = "/tmp/entry-aad-wireless.lock";  // one wireless start at a time
// One-shot double-MEDIA recall request: process id and monotonic timestamp, checked by the iDrive app.
constexpr char kMediaRecallFile[] = "/tmp/entry-aad-media-recall";
// One update from a USB stick at a time, the programs it holds for the car's loader to check, and the archive the last
// check offered (the install takes that one only).
constexpr char kUpdateLock[] = "/tmp/entry-aa-update.lock";
constexpr char kUpdateStaging[] = "/tmp/entry-aa-update";
constexpr char kUpdateChecked[] = "/tmp/entry-aa-update.checked";

// ---- control messages from the app to the service: a Unix datagram socket, abstract name (control_address.hpp) ----
constexpr char kControlName[] = "entry-aad-control";

}  // namespace contract
}  // namespace evo

#endif
