# Vehicle Telemetry

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
