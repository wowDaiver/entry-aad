#!/usr/bin/env python3
"""Private, pinned-SSH DSO upgrade/rollback. Never downloads or reboots."""

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shlex
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WORKSPACE = ROOT.parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("action", choices=("check", "upgrade", "rollback"))
    p.add_argument("--kit", type=Path, default=WORKSPACE / "vehicle-telemetry-continuous-20261116")
    p.add_argument("--old-kit", type=Path, default=WORKSPACE / "vehicle-telemetry-live-trial-20261116-r2")
    p.add_argument("--host", default="169.254.1.0")
    a = p.parse_args()
    if not re.fullmatch(r"169\.254\.(?:\d{1,3})\.(?:\d{1,3})", a.host) or any(int(x) > 255 for x in a.host.split(".")):
        p.error("Expected link-local IPv4")
    os.umask(0o077)
    a.kit = a.kit.resolve()
    a.old_kit = a.old_kit.resolve()
    sys.path.insert(0, str(a.kit))
    spec = importlib.util.spec_from_file_location("upgrade_host", a.kit / "host.py")
    h = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(h)
    h.local_check()
    h.HERE = a.old_kit
    h.local_check()
    h.HERE = a.kit
    config = json.loads((a.kit / "kit.json").read_text())
    activation = config.get("activation_policy", "single-start")
    network = config.get("egs_network_policy", "none")
    if network not in ("none", "vehicle-egs-hsfz-v1"):
        raise RuntimeError("Unknown EGS network policy")
    if activation not in ("single-start", "persistent-startup-guard"):
        raise RuntimeError("Unknown activation policy")
    out = (
        a.kit
        / "reports"
        / (datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-") + os.urandom(3).hex() + "-" + a.action)
    )
    out.mkdir(parents=True)
    print("REPORT:", out, flush=True)
    script = (ROOT / "upgrade146.sh").read_bytes()
    (out / "upgrade146.sh").write_bytes(script)
    (out / "operation.json").write_text(
        json.dumps(
            {
                "operation": a.action,
                "script_sha256": hashlib.sha256(script).hexdigest(),
                "old_dso_sha256": hashlib.sha256(
                    (a.old_kit / "payload/libVehicleTelemetry.so").read_bytes()
                ).hexdigest(),
                "new_dso_sha256": hashlib.sha256((a.kit / "payload/libVehicleTelemetry.so").read_bytes()).hexdigest(),
            },
            indent=2,
        )
        + "\n"
    )
    c = h.Connection(a.host)
    try:
        n = out / "new-stage"
        n.mkdir()
        new = h.stage(c, n)
        o = out / "old-stage"
        o.mkdir()
        h.HERE = a.old_kit
        try:
            old = h.stage(c, o)
        finally:
            h.HERE = a.kit
        h.write_json(out / "stages.json", {"new": new, "old": old})
        h.record(c.run("umask 077; cat >" + shlex.quote(new + "/upgrade146.sh"), data=script), out, "transfer-upgrader")
        base = "sh " + shlex.quote(new + "/upgrade146.sh") + " " + shlex.quote(new) + " " + shlex.quote(old)
        if a.action != "rollback":
            h.record(
                c.run(base + " check " + shlex.quote(activation) + " " + shlex.quote(network), timeout=50),
                out,
                "preflight",
            )
        if a.action != "check":
            wrapper = (
                'sh "$1/upgrade146.sh" "$1" "$2" "$3" "$4" "$5"; rc=$?; printf "%s\\n" "'
                '$rc" >"$1/upgrade.exit.tmp"; mv "$1/upgrade.exit.tmp" "$1/upgrade.exit'
                '"'
            )
            launch = (
                "nohup sh -c "
                + shlex.quote(wrapper)
                + " vt "
                + shlex.quote(new)
                + " "
                + shlex.quote(old)
                + " "
                + a.action
            )
            launch += (
                " "
                + shlex.quote(activation)
                + " "
                + shlex.quote(network)
                + " >"
                + shlex.quote(new + "/upgrade.log")
                + ' 2>&1 </dev/null & worker=$!; printf "WORKER=%s\\n" "$worker"; sleep 1'
            )
            h.record(c.run(launch), out, "launch")
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                r = c.run(
                    "if [ -f "
                    + shlex.quote(new + "/upgrade.exit")
                    + " ]; then cat "
                    + shlex.quote(new + "/upgrade.exit")
                    + "; else echo RUNNING; fi"
                )
                if r.returncode:
                    h.record(r, out, "poll-error")
                if r.stdout.strip() != b"RUNNING":
                    h.record(c.run("cat " + shlex.quote(new + "/upgrade.log")), out, "worker")
                    if r.stdout.strip() != b"0":
                        raise RuntimeError("Worker failed; preserve reports and inspect rollback/RO state")
                    break
                time.sleep(1)
            else:
                raise RuntimeError("Worker outcome unknown; do not repeat. Inspect saved stages/status first.")
        status_stage = old if a.action == "rollback" else new
        h.record(c.run("sh " + shlex.quote(status_stage + "/remote/control.sh") + " status"), out, "status-after")
        h.write_json(out / "result.json", {"operation_passed": True, "action": a.action, "reboot_sent": False})
    except BaseException as e:
        h.write_json(out / "result.json", {"operation_passed": False, "error": str(e)})
        raise
    finally:
        c.close()


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as e:
        print("STOP:", e, file=sys.stderr)
        sys.exit(1)
