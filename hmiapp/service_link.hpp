// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The app's side of the service: starting the supervisor /opt/aa/aa-supervisor from inside HmiMain3D, its control
// messages (UDP on 127.0.0.1), the state the service writes, and the stop sequence.
//
// The supervisor borrows the car's Bluetooth, runs the service entry-aad and gives the Bluetooth back when it ends.
// Control messages: ping, show, hide, quit, gear <n>, call <0|1>, daynight <n>, drivemode <n>, popup <0|1>.
// Single-threaded (the app thread), but for a reaper thread per start that only waits for the supervisor and logs.
#ifndef ENTRY_AAD_HMIAPP_SERVICE_LINK_HPP
#define ENTRY_AAD_HMIAPP_SERVICE_LINK_HPP

#include "service_state.hpp"

namespace evo {
namespace app {

// Starts the supervisor, unless it is missing, still running, was started less than 3 s ago, or starts are held for an
// update.
void launchService();
void sendToService(const char* message);
// The state file when its process is alive; else "connecting" while the supervisor prepares, "running" when the
// control port answers without a state file, "idle" otherwise.
ServiceState readServiceState();

// Stop: "quit", and STOP for the supervisor; then, one status poll after the other, SIGTERM at 3 s and SIGKILL at
// 15 s for the session process only. The supervisor always finishes its Bluetooth restore by itself.
void beginStop();
void serviceStop();  // once per status poll
bool stopInProgress();
// A supervisor is there, from its start to the end of its Bluetooth restore: it holds its lock, whoever started
// it. A second one would refuse to start.
bool supervisorRunning();

// The logs onto a USB stick (platform/log_copy.hpp): `aa-supervisor --copy-logs` in a child process, so that a slow
// stick never holds the app's thread. Starts one, unless one runs (false then, or when the start failed; logged).
bool startLogCopy();
bool logCopyRunning();
// Once per status poll: -1 while none ended since the last call, else its exit code (LogCopyResult), or -2 when it
// was stopped after kLogCopyLimitS or ended otherwise.
int logCopyEnded(long now);
const long kLogCopyLimitS = 60;

// The update from a USB stick (platform/update_install.hpp): `aa-supervisor --update-check` or `--update-install` in a
// child process, one at a time. False when one runs or the start failed (logged).
bool startUpdate(bool install);
// Once per status poll: -1 while none ended since the last call, else its exit code (UpdateResult), or -2 when it
// ended otherwise. A check still running after kUpdateCheckLimitS is stopped; an install never is.
int updateEnded(long now);
const long kUpdateCheckLimitS = 120;
// While held, launchService refuses: Android Auto is being stopped for an install, or the install runs.
void holdServiceStarts(bool hold);

}  // namespace app
}  // namespace evo

#endif
