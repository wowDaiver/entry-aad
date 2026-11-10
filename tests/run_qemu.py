# Copyright (c) 2026 Jiraiya43
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Run the ARM regression checks under QEMU, with the libraries of the copy of the car's system. Nothing else.

Build them first: bash toolchain/tc.sh make all test-arm-all. Logs and a summary go to artifacts/validation-pc.
"""

import json
import subprocess
import sys
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SYSROOT = Path.home() / "entrynav2/rootfs"
QEMU = [
    "qemu-arm",
    "-L",
    str(SYSROOT),
    "-E",
    "LD_LIBRARY_PATH=/opt/hmi/lib:/usr/local/lib:/usr/lib:/lib",
    "-E",
    "LD_BIND_NOW=1",
]
# Name, program (from the repository root), arguments, and what its output must hold (None: a PASS line or " OK").
CASES = [
    ("pb", "tests/test-pb-arm", [], None),
    ("sdr-g20", "tests/test-sdr-g20-arm", [], None),
    ("elf", "tests/test-evo-elf-arm", [], None),
    ("spp-snapshot", "tests/test-spp-snapshot-arm", [], None),
    ("kombi-phone", "tests/test-kombi-phone-arm", [], None),
    ("kombi-display", "tests/test-kombi-display-arm", [], None),
    ("kombi-graph", "tests/test-kombi-graph-arm", [], None),
    ("kombi-guide", "tests/test-kombi-guide-arm", [], None),
    ("kombi-client", "tests/test-kombi-client-arm", [], None),
    ("kombi-nav", "tests/test-kombi-nav-arm", [], None),
    ("kombi-golden", "tests/test-kombi-golden-arm", [], None),
    ("usb-transport", "tests/test-usb-transport-arm", [], None),
    ("transport-policy", "tests/test-transport-policy-arm", [], None),
    ("auto-supervisor", "tests/test-auto-supervisor-arm", [], None),
    ("usb-session-io", "tests/test-usb-session-io-arm", [], None),
    (
        "session-phone-usb",
        "tests/test-session-phone-arm",
        [str(ROOT / "tests/fixtures/session_phone.golden"), "--usb"],
        None,
    ),
    ("session-silence", "tests/test-session-silence-arm", [], None),
    ("gps-native", "tests/test-evo-gps-arm", [], None),
    ("video-presentation", "tests/test-video-presentation-arm", [], None),
    ("input", "tests/test-evo-input-arm", [], None),
    ("supervisor", "tests/test-supervisor-arm", [], None),
    ("wireless-lifecycle", "tests/test-wireless-lifecycle-arm", [], None),
    ("session-phone", "tests/test-session-phone-arm", [str(ROOT / "tests/fixtures/session_phone.golden")], None),
    ("wireless-phone", "tests/test-wireless-phone-arm", [str(ROOT / "tests/fixtures/wireless_phone.golden")], None),
    ("hmi-load", "tests/test-hmiapp-load-arm", [str(ROOT / "hmiapp/libEntryAad.so")], None),
    ("hold-machine", "tests/test-hold-machine-arm", [], None),
    ("spawn-glibc", "tests/test-spawn-attr-arm", ["--hu-glibc"], None),
    # The developer tools: the recorder's parser on 32 bits, and evo-check's loading with every symbol bound.
    ("capture-selftest", "evo-capture", ["--selftest"], None),
    ("check-load", "evo-check", ["--help"], "--dbus-watch"),
]


def passed(result, expected):
    """True when the program exited 0 and printed what its case expects."""
    if result.returncode != 0:
        return False
    if expected is not None:
        return expected in result.stdout
    return "PASS" in result.stdout or result.stdout.rstrip().endswith(" OK")


def main():
    out = ROOT / "artifacts/validation-pc"
    out.mkdir(parents=True, exist_ok=True)
    report = {"vehicle_connected": False, "visual_result": "NOT TESTED", "tests": {}}
    for name, program, args, expected in CASES:
        path = ROOT / program
        result = subprocess.run([*QEMU, str(path), *args], cwd=ROOT, capture_output=True, text=True, timeout=60)
        (out / f"{name}.log").write_text(result.stdout + result.stderr)
        if not passed(result, expected):
            raise SystemExit(f"FAIL ARM/QEMU {name} (exit {result.returncode})\n{result.stdout}{result.stderr}")
        report["tests"][name] = {"result": "PASS", "binary_sha256": sha256(path.read_bytes()).hexdigest()}
        print("PASS ARM/QEMU " + name, flush=True)
    # The car's own LayerManagerControl, with a stub in place of the compositor (tests/lm_cli_stub.c).
    result = subprocess.run([sys.executable, str(ROOT / "tests/test_real_lm_cli.py")], capture_output=True, text=True)
    (out / "lm-cli.log").write_text(result.stdout + result.stderr)
    if result.returncode != 0:
        raise SystemExit(f"FAIL ARM/QEMU lm-cli (exit {result.returncode})\n{result.stdout}{result.stderr}")
    report["tests"]["lm-cli"] = {"result": "PASS"}
    print("PASS ARM/QEMU lm-cli", flush=True)
    (out / "qemu.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
