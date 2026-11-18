#!/usr/bin/env python3
"""Read five firmware files over SSH; validate the known 146 identity before saving."""

import argparse
import hashlib
import json
import re
import subprocess
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parents[1]
KNOWN = ("opt/hmi/bin/HmiMain3D", "opt/hmi/lib/libAppFramework.A.so", "opt/hmi/lib/libCoreFramework.A.so")
PROVIDER = ("opt/app/usr/hmi/data/ro/hmiplugins/libhmiremoting.so", "opt/hmi/data/ro/hmiplugins/libhmiremoting.so")
SERVICE = "opt/hmi/lib/libhmil6services.so"
MAX_BYTES = 96 * 1024 * 1024
REMOTE = """set -eu
cd /
set -- opt/hmi/bin/HmiMain3D opt/hmi/lib/libAppFramework.A.so opt/hmi/lib/libCoreFramework.A.so
found=no
for p in opt/app/usr/hmi/data/ro/hmiplugins/libhmiremoting.so opt/hmi/data/ro/hmiplugins/libhmiremoting.so; do
    if [ -f "/$p" ]; then set -- "$@" "$p"; found=yes; break; fi
done
[ "$found" = yes ] || { echo "CDS provider path not found" >&2; exit 2; }
[ -f /opt/hmi/lib/libhmil6services.so ] || { echo "CDS service path not found" >&2; exit 2; }
set -- "$@" opt/hmi/lib/libhmil6services.so
exec tar -chf - "$@"
"""


def validate(stream, expected):
    files = {}
    with tarfile.open(fileobj=stream, mode="r:") as archive:
        for member in archive:
            if not member.isfile() or member.name not in (*KNOWN, *PROVIDER, SERVICE) or member.name in files:
                raise ValueError("unexpected archive entry: " + member.name)
            if member.size > 64 * 1024 * 1024:
                raise ValueError("oversized firmware file")
            body = archive.extractfile(member).read()
            if len(body) != member.size or not body.startswith(b"\x7fELF"):
                raise ValueError("short read or non-ELF firmware file")
            files[member.name] = body
    if len(files) != 5 or any(p not in files for p in (*KNOWN, SERVICE)) or sum(p in files for p in PROVIDER) != 1:
        raise ValueError("incomplete firmware capture")
    for path in KNOWN:
        if hashlib.sha256(files[path]).hexdigest() != expected[path]:
            raise ValueError("known 146 identity mismatch: " + path)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="169.254.1.0")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--plan", action="store_true", help="show read-only command; do not connect")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.:-]*", args.host):
        parser.error("invalid SSH hostname")
    if args.out.exists():
        parser.error("output must be a new directory")
    cmd = [
        "ssh",
        "-o",
        "StrictHostKeyChecking=yes",
        "-o",
        "ConnectTimeout=8",
        "-o",
        "NumberOfPasswordPrompts=1",
        "root@" + args.host,
        REMOTE,
    ]
    if args.plan:
        print(
            json.dumps(
                dict(
                    host=args.host,
                    remote_script=REMOTE,
                    output=str(args.out),
                    hu_writes=False,
                    restarts=False,
                    requires="parked car, HU awake, ENET, existing verified SSH host key",
                ),
                indent=2,
            )
        )
        return
    base = PROJECT / "research/evidence/hu-compatibility-2026-11-10/files"
    expected = {p: hashlib.sha256((base / p).read_bytes()).hexdigest() for p in KNOWN}
    with tempfile.TemporaryFile() as captured:
        subprocess.run(cmd, stdout=captured, check=True, timeout=45)
        if captured.tell() > MAX_BYTES:
            raise ValueError("archive exceeds total bound")
        captured.seek(0)
        files = validate(captured, expected)
    # Never call extractall: only validated regular file paths are written.
    args.out.mkdir(parents=True)
    rows = []
    for path, body in files.items():
        target = args.out / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(body)
        rows.append(dict(path=path, bytes=len(body), sha256=hashlib.sha256(body).hexdigest()))
    (args.out / "capture.json").write_text(
        json.dumps(
            dict(
                source="physical HU over SSH",
                identity_146_verified=True,
                files=rows,
                hu_writes=False,
                telemetry_values_read=0,
            ),
            indent=2,
        )
        + "\n"
    )
    print("CAPTURE_VERIFIED_146: " + str(args.out))


if __name__ == "__main__":
    main()
