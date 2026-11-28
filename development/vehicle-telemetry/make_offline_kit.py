#!/usr/bin/env python3
"""Build a private, self-contained folder; never contacts HU or internet."""

import argparse
import hashlib
import json
import os
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WORKSPACE = ROOT.parents[2]
DEFAULT = WORKSPACE / "vehicle-telemetry-offline"
ID = "vehicle-telemetry-146-v1"
ACTIONS = {
    "00-local-check.sh": "local-check",
    "01-check-and-capture.sh": "check",
    "02-install.sh": "install",
    "03-status.sh": "status",
    "04-collect.sh": "collect",
    "05-disable.sh": "disable",
    "06-recover-lock.sh": "unlock",
}


def checksums(base, paths, destination):
    destination.write_text(
        "".join(hashlib.sha256((base / name).read_bytes()).hexdigest() + "  " + name + "\n" for name in sorted(paths))
    )


def build(destination, refresh=False):
    destination = Path(destination)
    if destination.exists() and (not refresh or (destination / "reports").exists()):
        raise RuntimeError("Destination exists or contains reports; preserve it, use a new path.")
    report = json.loads((ROOT / "build/report.json").read_text())
    if report.get("egs_network_policy") == "internal-tester-assistant-v1":
        raise RuntimeError(
            "Internal transport also requires its worker and systemd unit. "
            "Use a private upgrade kit with install_most146.sh; legacy DSO-only packaging is blocked."
        )
    candidate = ROOT / "build/libVehicleTelemetry.146-candidate.so"
    source = ROOT / "build/native-menu"
    # Reject a stale resource bundle or source tree; do not silently package an older DSO.
    if candidate.read_bytes() != (source / "libVehicleTelemetry.so").read_bytes():
        raise RuntimeError("Rebuild native resources first.")
    for name in ("VehicleTelemetry.xml", "VehicleTelemetry.manifest"):
        if (ROOT / name).read_bytes() != (source / name).read_bytes():
            raise RuntimeError("Rebuild native resources: " + name)
    destination.mkdir(parents=True, exist_ok=True, mode=0o700)
    destination.chmod(0o700)
    names = []

    def copy_file(src, relative):
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, target)
        target.chmod(0o700 if relative.endswith(".sh") else 0o600)
        names.append(relative)

    for path in sorted((ROOT / "offline-src").rglob("*")):
        if path.is_file() and "__pycache__" not in path.parts:
            copy_file(path, str(path.relative_to(ROOT / "offline-src")))
    for name, action in ACTIONS.items():
        path = destination / name
        path.write_text('#!/bin/sh\nset -eu\ncd -- "$(dirname -- "$0")"\nexec python3 ./host.py ' + action + ' "$@"\n')
        path.chmod(0o700)
        names.append(name)
    # Same tar validator as the development collector, without its workspace-bound CLI.
    text = (ROOT / "collect146.py").read_text().split("\ndef main():", 1)[0]
    (destination / "capture.py").write_text(text)
    names.append("capture.py")
    for name in (
        "libVehicleTelemetry.so",
        "VehicleTelemetry.xml",
        "VehicleTelemetry.manifest",
        "VehicleTelemetry_Texts.zip",
        "VehicleTelemetry_Images.zip",
    ):
        copy_file(source / name, "payload/" + name)
    (destination / "payload/.vehicle-telemetry-owner").write_text(ID + "\n")
    names.append("payload/.vehicle-telemetry-owner")
    payload = [name.removeprefix("payload/") for name in names if name.startswith("payload/")]
    checksums(destination / "payload", payload, destination / "payload.sha256")
    names.append("payload.sha256")
    firmware = {
        "opt/hmi/bin/HmiMain3D": "6250c2b96cd4634cb82fe9bcdce3a9b6f95b25b28b55149ef7f38090d353822d",
        "opt/hmi/lib/libAppFramework.A.so": "737d7163710a6450ef33ed95d0dcb32283d49bb3cda99646e5c45fa4ce9d5d05",
        "opt/hmi/lib/libCoreFramework.A.so": "18a0af3221db808a23483fe4a3e9e536265ecf05f7bc2e201a4acefe822d1875",
    }
    (destination / "firmware.sha256").write_text(
        "".join(digest + "  " + path + "\n" for path, digest in firmware.items())
    )
    names.append("firmware.sha256")
    remote = [
        name
        for name in names
        if name.startswith(("remote/", "payload/")) or name in ("firmware.sha256", "payload.sha256")
    ]
    checksums(destination, remote, destination / "REMOTE_SHA256SUMS")
    names.append("REMOTE_SHA256SUMS")
    for name in ("LICENSE", "NOTICE.txt"):
        copy_file(source / name, name)
    copy_file(ROOT / "offline-tests/checks.log", "CHECKS.log")
    # Reuse this Mac's existing personal ENET credentials without printing their content.
    for name in ("hu-known_hosts", "hu-password.txt"):
        copy_file(WORKSPACE / "wifi-aa-offline" / name, name)
    config = dict(
        id=ID,
        firmware="002_146_002",
        firmware_sha256=firmware,
        candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
        vehicle_tested=False,
        installed_during_preparation=False,
        activation_policy=report.get("activation_policy", "single-start"),
        egs_network_policy=report.get("egs_network_policy", "none"),
        source_directory=str(ROOT),
        dependencies="Python 3, OpenSSH, sh already on this Mac",
        native_checks=dict(python_tests=report["python_tests"], sanitized_cpp_suites=report["sanitized_cpp_suites"]),
    )
    (destination / "kit.json").write_text(json.dumps(config, indent=2) + "\n")
    names.append("kit.json")
    checksums(destination, names, destination / "SHA256SUMS")
    for name in names:
        (destination / name).chmod(0o700 if name.endswith(".sh") else 0o600)
    print("PRIVATE_OFFLINE_FOLDER_READY:", destination)
    print("Files:", len(names), "; no vehicle/network connection; credentials not printed")
    return destination


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=DEFAULT)
    parser.add_argument("--refresh", action="store_true")
    args = parser.parse_args()
    os.umask(0o077)
    build(args.out, args.refresh)
