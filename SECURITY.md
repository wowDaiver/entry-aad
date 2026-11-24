# Security policy

## Reporting a vulnerability

Please report security issues privately, through GitHub: **Security** tab of this repository, then
**Report a vulnerability**. Do not open a public issue for them. Reports in English or French are welcome.

You can expect a first answer within a week. Once a fix is released, the advisory is published with credit to the
reporter, unless they prefer otherwise.

Only the latest release is supported: fixes are not backported.

## What this project touches

This project runs on a car's head unit. It is installed by hand over SSH on the car's ENET (OBD to Ethernet) cable, as
described in [docs/INSTALLATION.md](docs/INSTALLATION.md); later versions can also be installed from a USB stick, from
the iDrive menu. Things worth knowing when assessing a report:

| Area | Behaviour |
|---|---|
| Head unit root password | Not handled, stored or changed by this project; the user types it into their own SSH client. EntryNav2 units are known to share a default root password: treat SSH on the unit as unprotected and keep the ENET port physically controlled. |
| Changes on the car | Two new folders on the system partition and settings in `/var/local/data/AA`. No BMW file is modified. The system is writable only during the copy; after an update from a USB stick, until the restart that follows a few seconds later (the iDrive keeps its replaced app open until then). |
| Network on the car | The Android Auto service listens on the address of the car's Wi-Fi access point (on every interface if that address is not up yet) and accepts connections from that network only; the session is TLS-encrypted as Android Auto requires. The iDrive app talks to the service over a local Unix socket (abstract name, no network port). Guidance goes to the instrument cluster over SOME/IP on the car's internal network. |
| Release integrity | Each release archive comes with its SHA-256 checksum, and its files with MD5 sums checked on the car before and after the copy. From 0.13.0-rc.10, it also holds `UPDATE`, the SHA-256 of every installed file, signed with the project's RSA 3072-bit key (`UPDATE.sig`, PKCS #1 v1.5). The update from a USB stick only installs an archive with a valid signature by a key built into the installed programs (`platform/update_key.hpp`); the private key never leaves the maintainer's PC. An installation by SSH does not check the signature: download archives only from this repository's releases. |
| Update from a USB stick | Started from the iDrive menu by two presses, never by plugging a stick in; the second press installs exactly the archive the first one checked, never during a call or in reverse gear. Everything is checked before anything is written, programs against the car's ELF format and its own loader; new files are written beside the old ones and renamed over them one by one, with a journal on the data partition to finish an interrupted swap. Only the project's two folders are touched. |

## Scope

In scope: this repository's code, its installation procedure and the archives published in its releases.

Out of scope: vulnerabilities in BMW's own software, in Android Auto or in the phone, unless this project makes them
reachable or worse. Please report those to their vendors.
