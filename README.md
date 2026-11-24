<div align="center">

# EntryNav2 (EntryEvo) Android Auto

**Wireless Android Auto for BMW iDrive on the EntryNav2 head unit.**<br>
No dongle, no cable needed, no BMW file replaced.

[![CI](https://github.com/Jiraiya43/EntryNav2-Android-Auto/actions/workflows/ci.yml/badge.svg)](https://github.com/Jiraiya43/EntryNav2-Android-Auto/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Jiraiya43/EntryNav2-Android-Auto?include_prereleases&sort=semver)](https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases)
[![License](https://img.shields.io/badge/license-PolyForm%20Noncommercial%201.0.0-blue)](LICENSE.md)

English · [Français](README.fr.md)

</div>

> [!WARNING]
> This is experimental software that writes to your car's head unit. It is developed on **one car**; owners of about
> twenty others report what they saw ([list](docs/COMPATIBILITE.md), in French), and nothing guarantees that it works on
> yours. Read [Status](#status) and the [disclaimer](#licence-and-disclaimer) first. It is not affiliated with BMW or
> Google.

## Features

- **Wireless Android Auto.** The phone pairs over Bluetooth, then joins the car's own Wi-Fi (the one CarPlay uses).
- **A USB cable works too** (experimental, off by default), for phones without wireless Android Auto: a checkbox in
  the menu picks the cable or wireless, nothing is guessed. [Setup and limits](docs/USB.md) (in French).
- **Started from iDrive.** An *Android Auto* entry in the Media menu, with its own menu: start, stop, start
  automatically, logs, update, help.
- **Updates without a PC** (from 0.13.0-rc.10). After the first installation over SSH, a new version installs from a
  USB stick, through the Android Auto menu. Only an archive signed by the project is accepted
  ([updating](docs/INSTALLATION.md#par-clé-usb-sans-pc), in French).
- **Original controls.** iDrive controller, hard keys, touch screen, steering wheel buttons, voice button for the
  Assistant.
- **Car microphone** for the Google Assistant, **car speakers** over Bluetooth.
- **Car GPS** sent to the phone, with a fallback to the phone's own GPS.
- **Turn-by-turn on the head-up display and cluster**: arrow, distance, street name, roundabout exit (G series; F
  series being tried, [compatibility](docs/COMPATIBILITE.md#flèches-sur-le-compteur-des-série-f)).
- **Lives alongside iDrive**: reversing camera, volume overlay, driving modes, day and night.
- **Always reversible.** Bluetooth goes back to iDrive when Android Auto stops, and a guard switches the app off if
  iDrive crashes several times in a row after loading it.

## Status

| | |
|---|---|
| Latest release | [0.12.0](https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/latest), tried in a car (pre-1.0: expect rough edges) |
| Tried in a car | the reference car, a 2019 BMW G20 (iDrive software `202111011321`) |
| Other cars | reported working by their owners: G20, G21, X3 G01, F20, F21, F30, F40, F45, X1 F48, i3, MINI F54, F56, F57 ([list](docs/COMPATIBILITE.md#voitures-essayées)); a compatibility check is built in |

Every change is tested on a PC, and the ARM programs run under QEMU against the car's own libraries. Only a car test
counts as proof: [CHANGELOG.en.md](CHANGELOG.en.md) says for each version whether it was tried in a car or only tested
on a PC. The [French changelog](CHANGELOG.md) is the authoritative version.

## Requirements

- A BMW with an **EntryNav2** head unit (called *EntryEVO* inside its system), with a **1280×480** or **800×480**
  screen ([screens](docs/COMPATIBILITE.md#autres-tailles-décran)). Wireless needs the unit's **CarPlay Wi-Fi**;
  without it, a [USB cable](docs/USB.md) works too.
  NBT Evo, ID7 and MGU units run other systems: this project cannot run on them.
- For the first installation, a PC with an SSH client (Windows 10/11, Linux or macOS) and an **ENET cable** (OBD to
  Ethernet). Later updates install from a USB stick.
- The head unit's **root password**. It is not provided with this project.
- An Android phone with Android Auto.

## Installing

Installation is a short manual procedure over SSH, described step by step in
[docs/INSTALLATION.md](docs/INSTALLATION.md) (in French; the commands speak for themselves):

1. Download `entrynav2-android-auto-<version>-armhf.tar` from [Releases](https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases) and check
   its SHA-256.
2. Copy it to the head unit's `/tmp` with `scp`.
3. **Check the car**: nothing is written. The unit's own loader confirms every library and function the programs need,
   and `aa-supervisor --compat` rehearses the startup of Android Auto.
4. **Install**: the system is remounted read-write only while two folders are copied (`/opt/aa` and
   `/opt/hmi/data/ro/Plugins/EntryAad`), then the unit restarts.
5. On iDrive: *Media*, *Android Auto*, *Démarrer*.

After that, newer versions install without a PC: the archive copied as is onto a USB stick, then *Mettre à jour par
clé USB* (*Update from USB stick*) in the Android Auto menu. Only an archive signed by the project is accepted,
everything is checked before anything is written, and an update cut by a power failure is finished by the next one
([updating](docs/INSTALLATION.md#mettre-à-jour)).

Settings and logs live in `/var/local/data/AA` ([docs/MAINTENANCE.md](docs/MAINTENANCE.md)). Uninstalling removes the two
folders.

## How it works

```mermaid
flowchart LR
    P[Android phone]
    subgraph HU[EntryNav2 head unit]
        A[iDrive app<br/>libEntryAad.so] --> T[Supervisor<br/>aa-supervisor]
        T --> S[Service<br/>entry-aad]
        S --> V[H.264 on screen<br/>libxsvideo + LayerManager]
        I[Controller, keys, touch] --> S
    end
    P -- Bluetooth: Wi-Fi credentials --> S
    P -- Wi-Fi: encrypted Android Auto --> S
    S -- SOME/IP: guidance --> K[Cluster and HUD]
```

1. The iDrive app starts the supervisor, which borrows a Bluetooth slot and switches the car's Wi-Fi on.
2. The service sends the Wi-Fi credentials to the phone over Bluetooth; the phone connects back over Wi-Fi with TLS.
3. The service describes the car, decodes the phone's H.264 video with the unit's hardware decoder and shows it in its
   own LayerManager layer.
4. Input, GPS, day and night, microphone and guidance each travel on their own channel.

Details in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Building from source

You need Linux or WSL with Docker, and for the ARM build **a copy of the car's system** in `~/entrynav2/rootfs`
(`HU_ROOTFS` to change it). That copy holds BMW files: it never goes into git. Without it, the PC tests still run, as
in CI.

```sh
make check                                              # style, content checks, PC tests: what CI runs
docker build -t entrynav2-tc:4.7 toolchain              # once: GCC 4.7 for ARMv7 hard-float
bash toolchain/tc.sh make -j4 all hmiapp test-arm-all   # ARM programs, linked against the car's libraries
python3 -I tests/run_qemu.py                            # ARM tests under QEMU with the car's libraries
make verify_binary verify_wireless                      # record the programs that passed
python3 tools/package_release.py --unsigned             # artifacts/entrynav2-android-auto-<version>-armhf.tar
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full test list and the project's rules.

## Repository layout

```text
core/        Android Auto protocol: session, TLS, Bluetooth-to-Wi-Fi handshake, car description
platform/    everything specific to EntryNav2: video, layers, input, GPS, microphone, Wi-Fi, cluster
hmiapp/      the iDrive app loaded by HmiMain3D
tools/       supervisor, release packaging and checks
tests/       PC tests and ARM tests under QEMU
toolchain/   Docker image of the cross compiler
docs/        documentation (French)
```

## Contributing

Bug reports, compatibility reports from other cars and pull requests are welcome: see
[CONTRIBUTING.md](CONTRIBUTING.md) and the [code of conduct](CODE_OF_CONDUCT.md). A compatibility report (the check of
step 3) changes nothing on the car and helps the most. Security issues: see [SECURITY.md](SECURITY.md).

## Supporting the project

This project is free and stays free; nothing is expected in return. If you want to support it, you can do so through
[GitHub Sponsors](https://github.com/sponsors/Jiraiya43). Support will go towards fitting an MGU head unit (iDrive 7)
to my G20 and offering open-source apps for iDrive 7.

## Credits

This project started as a port of [Supra-iDrive6-Android-Auto](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto) by
Sq00ky, then was reworked and extended for the EntryNav2. Thanks to Sq00ky for that work.

## Licence and disclaimer

© 2026 Jiraiya43, under the [PolyForm Noncommercial License 1.0.0](LICENSE.md): any noncommercial use, including
modifying and sharing, as long as the licence goes with every copy. Commercial use is not allowed.

The software comes as is, without any warranty. Installing it modifies the head unit's system and may affect your
warranty. Use it only on a car you own or with its owner's consent, with the car parked during installation, and
never operate the head unit's shell while driving.

BMW and iDrive are trademarks of BMW AG; Android and Android Auto are trademarks of Google LLC. This project is not
affiliated with, endorsed or sponsored by either.
