# Copyright (c) 2026 Jiraiya43
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Build the release archive: artifacts/entrynav2-android-auto-<version>-armhf.tar, ready to copy to the head unit.

Its files sit at the paths they take on the car (opt/aa/..., opt/hmi/data/ro/Plugins/EntryAad/...), with MD5SUMS
checkable by the car's BusyBox before and after the copy (docs/INSTALLATION.md). Only the programs recorded by the last
validation (make verify_binary verify_wireless) go in. The archive is reproducible: fixed owner, modes and dates.

For the update from a USB stick (platform/update_package.hpp) it also holds UPDATE, the list of the installed files with
their mode, size and SHA-256, and UPDATE.sig, its signature by the project's key: RSA PKCS #1 v1.5 over SHA-256, the
same bytes every time, so the archive stays reproducible. The signature is checked against the keys the programs trust
(platform/update_key.hpp) before anything is written. The key is read from .secrets/update-signing-key.pem (never in
git) or --key. Without it, --unsigned builds an archive without UPDATE.sig: it installs by SSH only.

Usage: package_release.py [--key PEM | --unsigned]
"""

import argparse
import base64
import io
import json
import re
import subprocess
import sys
import tarfile
import tempfile
from hashlib import md5, sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLUGIN = "opt/hmi/data/ro/Plugins/EntryAad"
PLUGIN_FILES = (
    "libEntryAad.so",
    "EntryAad.manifest",
    "EntryAad.xml",
    "EntryAad_ALL_Texts.zip",
    "EntryAad_ALL_Images.zip",
)
PROGRAMS = ("entry-aad", "aa-supervisor")
EXECUTABLES = {f"opt/aa/{name}" for name in PROGRAMS} | {f"{PLUGIN}/libEntryAad.so"}
SIGNING_KEY = ROOT / ".secrets/update-signing-key.pem"
UPDATE_FIRST_LINE = "entrynav2-android-auto update 1"


def validated_programs() -> dict:
    """The ARM programs, exactly those recorded by the last validation."""
    report_path = ROOT / "artifacts/wireless-validation.json"
    if not report_path.is_file():
        raise SystemExit(f"FAIL: {report_path} missing: run make verify_binary verify_wireless first")
    report = json.loads(report_path.read_text())
    programs = {}
    for name in PROGRAMS:
        data = (ROOT / name).read_bytes()
        if report["binaries"][name]["sha256"] != sha256(data).hexdigest():
            raise SystemExit(f"FAIL: {name} differs from the validated one: rebuild, then make verify_wireless")
        programs[f"opt/aa/{name}"] = data
    return programs


def update_list(version: str, files: dict) -> bytes:
    """UPDATE: every file the archive installs (those under opt/), as platform/update_package.hpp reads it."""
    lines = [UPDATE_FIRST_LINE, f"version {version}"]
    for name, data in sorted(files.items()):
        if name.startswith("opt/"):
            mode = "755" if name in EXECUTABLES else "644"
            lines.append(f"file {mode} {len(data)} {sha256(data).hexdigest()} {name}")
    return ("\n".join(lines) + "\n").encode()


def project_keys() -> list:
    """The DER public keys the programs trust, read from platform/update_key.hpp."""
    text = (ROOT / "platform/update_key.hpp").read_text()
    bodies = re.findall(r"const unsigned char kUpdateKey\d+\[\] = \{(.*?)\};", text, re.S)
    return [bytes(int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})", body)) for body in bodies]


def pem(der: bytes) -> bytes:
    lines = base64.b64encode(der).decode()
    body = "\n".join(lines[i : i + 64] for i in range(0, len(lines), 64))
    return f"-----BEGIN PUBLIC KEY-----\n{body}\n-----END PUBLIC KEY-----\n".encode()


def sign(data: bytes, key: Path, trusted=None) -> bytes:
    """data's signature by key, after checking that one of the trusted keys (the programs' by default) verifies it."""
    signature = subprocess.run(
        ["openssl", "dgst", "-sha256", "-sign", str(key)], input=data, capture_output=True, check=True
    ).stdout
    with tempfile.TemporaryDirectory() as tmp:
        signature_file = Path(tmp) / "UPDATE.sig"
        signature_file.write_bytes(signature)
        for number, der in enumerate(project_keys() if trusted is None else trusted):
            public = Path(tmp) / f"key{number}.pem"
            public.write_bytes(pem(der))
            check = ["openssl", "dgst", "-sha256", "-verify", str(public), "-signature", str(signature_file)]
            if subprocess.run(check, input=data, capture_output=True).returncode == 0:
                return signature
    raise SystemExit("FAIL: the signing key is not one the programs trust (platform/update_key.hpp)")


def release_files(version: str, key) -> dict:
    subprocess.run(
        [sys.executable, "-I", str(ROOT / "hmiapp/make_db.py"), str(ROOT / "hmiapp/res"), str(ROOT / "hmiapp")],
        check=True,
    )
    files = {f"{PLUGIN}/{name}": (ROOT / "hmiapp" / name).read_bytes() for name in PLUGIN_FILES}
    if not files[f"{PLUGIN}/libEntryAad.so"].startswith(b"\x7fELF"):
        raise SystemExit("FAIL: hmiapp/libEntryAad.so is not an ARM build")
    files.update(validated_programs())
    files["MD5SUMS"] = "".join(f"{md5(data).hexdigest()}  {name}\n" for name, data in sorted(files.items())).encode()
    files["UPDATE"] = update_list(version, files)
    if key is not None:
        files["UPDATE.sig"] = sign(files["UPDATE"], key)
    files["LICENSE.md"] = (ROOT / "LICENSE.md").read_bytes()
    files["INSTALLATION.md"] = (ROOT / "docs/INSTALLATION.md").read_bytes()
    return files


def write_archive(archive: Path, top: str, files: dict, epoch: int) -> str:
    """The USTAR archive of files under top, with every folder given explicitly. Returns its SHA-256."""
    payload = io.BytesIO()
    folders = sorted({str(Path(name).parent).replace("\\", "/") for name in files} - {"."})
    folders = sorted({"/".join(folder.split("/")[: i + 1]) for folder in folders for i in range(folder.count("/") + 1)})
    with tarfile.open(fileobj=payload, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for name in [""] + folders:  # explicit folders: no tar has to guess their mode
            item = tarfile.TarInfo(f"{top}/{name}".rstrip("/"))
            item.type = tarfile.DIRTYPE
            item.mode = 0o755
            item.mtime = epoch
            item.uname = item.gname = "root"
            tar.addfile(item)
        for name, data in sorted(files.items()):
            item = tarfile.TarInfo(f"{top}/{name}")
            item.size = len(data)
            item.mode = 0o755 if name in EXECUTABLES else 0o644
            item.mtime = epoch
            item.uname = item.gname = "root"
            tar.addfile(item, io.BytesIO(data))
    archive.write_bytes(payload.getvalue())
    return sha256(payload.getvalue()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description="Build the release archive.")
    choice = parser.add_mutually_exclusive_group()
    choice.add_argument("--key", type=Path, help=f"the project's signing key (default {SIGNING_KEY})")
    choice.add_argument("--unsigned", action="store_true", help="no UPDATE.sig: the archive installs by SSH only")
    args = parser.parse_args()
    key = None if args.unsigned else (args.key or SIGNING_KEY)
    if key is not None and not key.is_file():
        raise SystemExit(f"FAIL: no signing key {key}: pass --key, or --unsigned for an archive installed by SSH only")
    version = (ROOT / "VERSION").read_text().strip()
    top = f"entrynav2-android-auto-{version}"
    epoch = int(
        subprocess.run(["git", "log", "-1", "--format=%ct"], cwd=ROOT, capture_output=True, text=True).stdout or 0
    )
    archive = ROOT / f"artifacts/{top}-armhf.tar"
    archive.parent.mkdir(exist_ok=True)
    digest = write_archive(archive, top, release_files(version, key), epoch)
    (archive.parent / f"{archive.name}.sha256").write_text(f"{digest}  {archive.name}\n")
    print(f"{archive}\nsha256 {digest}")
    print("signed by the project's key: installs from a USB stick" if key else "UNSIGNED: installs by SSH only")


if __name__ == "__main__":
    main()
