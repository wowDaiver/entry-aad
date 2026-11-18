#!/usr/bin/env python3
"""Restart only HMI in the fixed local telemetry VM. No car endpoints exist here."""

import argparse
import hashlib
import json
import shlex
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument("--next-plugin", action="store_true")
parser.add_argument("--verbose", action="store_true")
args = parser.parse_args()
env = {
    "XDG_RUNTIME_DIR": "/run/system/0",
    "DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/session_bus_socket",
    "PCL_LAB_FIXTURES": "/qemu-pcl/fixtures",
    "OWNER146_LANGUAGE": "17",
    "VEHICLE_TELEMETRY_EMULATOR": "isolated-owner146",
    "LD_LIBRARY_PATH": (
        "/qemu-fast/usr/lib/arm-linux-gnueabihf:/qemu-fast/lib/arm-linux-gnueab"
        "ihf:/qemu-wayland/lib:/qemu-software/lib:/usr/local/lib:/opt/hmi/lib:/"
        "usr/lib:/lib"
    ),
    "LIBGL_DRIVERS_PATH": "/qemu-fast/usr/lib/arm-linux-gnueabihf/dri",
    "__EGL_VENDOR_LIBRARY_FILENAMES": "/qemu-fast/usr/share/glvnd/egl_vendor.d/50_mesa.json",
    "GALLIUM_DRIVER": "virgl",
    "HMI_LAB_FPS": "10",
    "EGL_PLATFORM": "wayland",
    "DLT_LOCAL_PRINT_MODE": "FORCE_ON" if args.verbose else "FORCE_OFF",
    "LD_PRELOAD": (
        "/qemu-integration/libowner146-language.so:/qemu-integration/libframe-t"
        "iming.so:/qemu-integration/libshaderlab.so:/qemu-pcl/libpcl-fixture.so"
    ),
}
# Keep explicit demo mode consistent with this VM's prepared initrd.
report = HERE / "build/prepare.json"
if report.exists() and json.loads(report.read_text()).get("synthetic_telemetry") is True:
    env["VEHICLE_TELEMETRY_DEMO"] = "battery-and-temperatures"
cmd = ""
if args.next_plugin:
    digest = hashlib.sha256((HERE / "build/libVehicleTelemetry.emulator.so").read_bytes()).hexdigest()
    cmd += (
        "if test \"$(openssl dgst -sha256 /tmp/vehicle-telemetry-next.so | awk '{print $NF}')\" = " + digest + "; then "
    )
cmd += (
    'pids=""; for maps in /proc/[0-9]*/maps; do if grep -q /opt/hmi/bin/Hmi'
    'Main3D "$maps" 2>/dev/null; then p=${maps#/proc/}; p=${p%/maps}; pids='
    '"$pids $p"; fi; done; '
)
cmd += 'set -- $pids; if [ "$#" != 1 ]; then echo EXPECTED_ONE_LOCAL_HMI; false; else kill -9 "$1"; '
if args.next_plugin:
    cmd += (
        "mv /tmp/vehicle-telemetry-next.so /opt/hmi/data/ro/Plugins/VehicleTele"
        "metry/libVehicleTelemetry.so; chmod 755 /opt/hmi/data/ro/Plugins/Vehic"
        "leTelemetry/libVehicleTelemetry.so; "
    )
cmd += "nohup env " + " ".join(shlex.quote(k + "=" + v) for k, v in env.items())
cmd += (
    " /qemu-fast/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3 /opt/hmi/bin/H"
    "miMain3D -ro=/opt/hmi/data/ro -rw=/var/local/data/HMI -ibabfroot=/opt/"
    "iba >/tmp/telemetry-hmi.log 2>&1 </dev/null & echo TELEMETRY_LOCAL_HMI"
    "_PID=$!; fi"
)
if args.next_plugin:
    cmd += "; else echo UPLOAD_SHA_MISMATCH; false; fi"
subprocess.run([sys.executable, str(HERE / "console.py"), cmd], check=True)
