#!/usr/bin/env python3
"""Fixed loopback serial endpoint of the telemetry VM; no SSH or car support."""

import argparse
import base64
import hashlib
import re
import select
import shlex
import socket
import time
import uuid
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("command", nargs="?")
parser.add_argument("--save", type=Path)
parser.add_argument("--upload", nargs=2, metavar=("LOCAL", "GUEST"))
args = parser.parse_args()
commands = [args.command] if args.command else []
if args.upload:
    source, guest = args.upload
    encoded = base64.b64encode(Path(source).read_bytes()).decode()
    target = shlex.quote(guest + ".b64")
    commands = [": > " + target] + [
        "printf '%s' " + shlex.quote(encoded[i : i + 768]) + " >> " + target for i in range(0, len(encoded), 768)
    ]
    commands += [
        "openssl base64 -d -A -in " + target + " -out " + shlex.quote(guest),
        "openssl dgst -sha256 " + shlex.quote(guest),
    ]
if not commands:
    parser.error("command or --upload required")
commands = ["set +H"] + commands  # the lab serial shell is interactive Bash
results = []
with socket.create_connection(("127.0.0.1", 17703), timeout=3) as conn:
    conn.setblocking(False)
    for command in commands:
        token = uuid.uuid4().hex
        marker = ("TELEMETRY_VM_DONE_" + token).encode()
        # Split input prefix/token so TTY echo cannot look like completion.
        pending = memoryview(
            (command + "\nvt_rc=$?; printf '\\nTELEMETRY_VM_DONE_%s:%s\\n' '" + token + '\' "$vt_rc"\n').encode()
        )
        output = bytearray()
        deadline = time.monotonic() + 45
        pattern = re.compile(re.escape(marker) + rb":([0-9]+)\r?\n")
        while not pattern.search(output):
            if time.monotonic() > deadline:
                raise TimeoutError("telemetry VM serial command did not complete")
            readable, writable, _ = select.select([conn], [conn] if pending else [], [], 1)
            if writable:
                count = conn.send(pending[:2048])
                pending = pending[count:]
            if readable:
                body = conn.recv(65536)
                if not body:
                    raise RuntimeError("VM console disconnected")
                output.extend(body)
            if len(output) > 8 * 1024 * 1024:
                raise RuntimeError("diagnostic output limit exceeded")
        results.append(bytes(output).split(marker)[0])
        code = int(pattern.search(output).group(1))
        if code:
            raise RuntimeError("VM command failed: " + str(code) + "\n" + results[-1].decode(errors="replace"))
body = b"".join(results)
if args.save:
    args.save.parent.mkdir(parents=True, exist_ok=True)
    args.save.write_bytes(body)
if args.upload:
    digest = hashlib.sha256(Path(source).read_bytes()).hexdigest()
    if digest.encode() not in body:
        raise RuntimeError("VM upload SHA256 mismatch")
    print("Uploaded and SHA256 verified in isolated VM:", guest, "bytes:", Path(source).stat().st_size)
else:
    print(body.decode(errors="replace"), end="")
