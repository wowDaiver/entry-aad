# Vehicle Telemetry 0.2.0 — EntryNav2 146

Experimental standalone native instrument plugin for `002_146_002`. It does not change the Android Auto daemon.
The module keeps its GPL-3.0-or-later license; the enclosing Android Auto project retains its own license.

The dashboard has coolant/oil temperature, RPM, speed, engine load and torque, calculated metric horsepower,
voltage/current/SoC, D/S/M gear mode, transmission temperature, parking brake and interval-based recuperation.
Unavailable inputs remain unknown. The physical build excludes emulator fixtures and archived recordings.

## Verified scope

Installed and initialized on one owner146 head unit. Dashboard, SoC/torque/power and parking-brake switching
were confirmed by the owner. The parking-brake signal was read in both states. Recuperation was inactive
in the stationary observation window; agreement with the native Eco Pro indicator while driving is unverified.
RPM/speed subscriptions and the graphic frame limit are 200 ms; actual rates depend on CDS and rendering.

DDE12 must identify as `0F1FF0`, EGS18 as `0F21D0`. Reads are sequential, nonblocking and allowlisted,
with fixed frame/size/deadline limits. No diagnostic sessions, security access, ECU writes or actuation are used.
The physical build uses the native internal TesterAssistent service through an isolated VMOST worker.
There is no external Ethernet diagnostic fallback. The internal envelope and all 17 diagnostic records
were verified on the parked owner146 vehicle. Startup after a full vehicle sleep remains a separate live check.

- SoC: DDE43C2, separate from CDS255; torque: DDE48A5; horsepower requires fresh torque and CDS RPM.
- Sport: DA2E D with a fresh DA27 M/S gate; manual M takes precedence.
- Parking brake: DA31, BE16 0/1; invalid values do not become a brake state.
- Recuperation: first BE32 of the full 16-byte DDE40C0 record counts IGR charge in 0.1 As. Activity requires
  an increase between two valid observations, 0 < dt <= 3 seconds. The first observation, counter reset,
  wrap, error or long gap leaves activity unknown. Expired active status is hidden. Ordinary battery charging,
  P, voltage and ECO PRO alone do not establish recuperation.

## Checks without a car or BMW files

Install a C++11 compiler, Python 3, FreeType development files, zlib and a local TrueType font. On Debian:
`apt-get install clang libfreetype6-dev zlib1g-dev pkg-config fonts-dejavu-core python3`.

From the repository root:

```sh
python3 development/vehicle-telemetry/check_host.py --font /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
```

On macOS the runner also discovers LLVM, XQuartz FreeType and an installed system font. No font is distributed.
Checks cover 64 Python tests, 12 core ASan/UBSan suites, the isolated worker, installation/rollback rehearsals,
the physical graphics controller with a host font and PNG rendering. Factory/ARM checks require private owner inputs;
the 146 firmware Build ID/vtable gate is not replaced by the host stubs.

`dashboard/offline_mapping.py` replays eight sanitized captures separately through the native decoder and renderer.
The fixtures contain only fixed, non-identifying telemetry responses; archived snapshots are not live readings
and are never merged into a synchronous vehicle state. See [mapping details](dashboard/OFFLINE-MAPPING-RU.md).

## Owner-only build and deployment sources

`build.py`, `dashboard/build146.py`, `emulator/`, `collect146.py`, `offline-src/`, `offline-tests/`,
`make_offline_kit.py`, `upgrade146.*` and `bundle146.sh` preserve the development/build/recovery procedures.
Their full ARM, emulator and installer paths require a separately prepared owner workspace: exact firmware
libraries and menu evidence under the ignored `research/` tree, ABI inputs under `development/abi-146`,
and the adjacent private `entrynav2-qemu` lab/toolchain. They are not a self-contained public installation kit.
Private kits, passwords, host keys, firmware, fonts, VM images, binaries and vehicle logs are not committed.

Physical firmware is gated by exact Build IDs and imports. The persistent startup guard blocks failed starts;
updates preserve previous mapped files/inodes, publish registration last and restore root RO. Installation
requires the matching private kit, a parked car and a stopped AA service. Review rollback before deployment.

Source formats: pinned `emdzej/ediabasx-docs-sgbd` commit
`b644de8fbfbb4b207f57794e3c7894dc1dc58627`; the GPL profile gate provenance is in `vendor/kizapp/SOURCE.json`.
No BMW library, font or original firmware asset is part of this module.

## Internal diagnostic transport

The dashboard connects to `/run/vehicle-telemetry/most.sock`. The worker authenticates the local peer,
checks the exact HMI/AppFramework/CoreFramework firmware IDs and accepts only the existing DDE/EGS reads.
Each ECU must pass its F150 identity check in every client session before measurements are forwarded.
The worker constructs the native TesterAssistent envelope itself: FBlock 6, instance F0, channel A (function 50),
source 63, routine 0F0B containing only an allowlisted read. Neither caller-provided routines nor ECU writes are accepted.

Only one request may be outstanding. Responses must match channel, source, ECU, DID and exact length.
Closing the dashboard closes the local connection and bus subscription. A reply left over from a closed client
is discarded; an uncertain transaction triggers a 30-second quarantine and a new bus connection.
Unavailable or stale measurements keep the existing unknown/expiry behavior.
`/var/local/data/VehicleTelemetry146/most.status` records the boot ID, connection state and validated-record mask.

`build.py` builds the separate ARM worker as well as the plugin. To use private inputs with this public checkout,
set `VEHICLE_TELEMETRY_OWNER_ROOT` to the prepared owner workspace before running `build.py` and `dashboard/build146.py`.
The worker belongs at `/opt/vehicle-telemetry/vehicle-telemetry-most`; the accompanying systemd unit starts it
with `focussed.target`. It does not open the diagnostic bus until the dashboard asks for a read.

`install_most146.sh` runs from `remote/` of a private kit containing the existing verified `common.sh`,
`firmware.sha256`, `payload.sha256`, `REMOTE_SHA256SUMS`, unchanged menu resources in `payload/`, the new plugin,
and the worker/unit in `transport/`. It supports `check`, `install` and `rollback`, pins the known predecessor,
preserves its mapped inode, checks every write and restores root RO. Stop AA and any existing transport worker first;
reload systemd after installation and reboot the HU to load the plugin. No BMW binary or AA setting is changed.
The legacy DSO-only kit generator rejects this build because it cannot install the required worker and unit.
