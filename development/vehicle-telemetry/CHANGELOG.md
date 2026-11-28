# Vehicle Telemetry

## 0.2.0 - 2026-11-25

- Replace the cable-activated external Ethernet diagnostic route with the native internal TesterAssistent channel.
- Isolate the process-global VMOST library in a separate, firmware-gated service, reached through a private Unix socket.
- Retain the DDE/EGS identity gates and exact read allowlist; stop bus activity when the instrument page closes.
- Discard replies from closed clients and quarantine uncertain transactions before reconnecting.
- Add real-worker sanitizer regressions and transactional worker/unit/plugin installation with rollback rehearsals.

All 17 diagnostic records answered through the internal channel on the parked owner146 vehicle.
Cold startup after a full vehicle sleep still requires physical validation.

## 0.1.0 - 2026-11-18

- Native instrument dashboard for EntryNav2 002_146_002, isolated from Android Auto.
- CDS engine temperature, oil temperature, RPM and speed; 200 ms RPM/speed subscriptions and frame limit.
- Profile-gated DDE/EGS reads, sequential nonblocking transport and bounded recovery.
- DDE battery SoC and torque, calculated metric horsepower, and distinct D/S/M lever modes.
- EGS parking brake, verified in both physical states and in the installed interface.
- Recuperation activity from the dedicated DDE IGR charge counter over a fresh interval.
- Native graphics, text pages, persistent startup guard and transactional update/rollback sources.
- Offline source mapping, sanitized wire fixtures and host sanitizer checks.

Installed and initialized on one owner146 head unit. The owner confirmed the dashboard and parking-brake
switching. Recuperation was observed inactive while stationary; agreement with active Eco Pro indication
while driving is not yet verified. This is an experimental vehicle-specific module, not a general firmware release.
