# SPDX-License-Identifier: GPL-3.0-or-later
"""Rehearse transaction/rollback with a disposable filesystem and mount double."""

import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OLD = "728c735cd7a45b05da671e5f3c0340fa6002d538625dd0103daaf00b848666ea"


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


class InstallTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.fs = self.base / "root"
        self.kit = self.base / "kit"
        self.remote = self.kit / "remote"
        self.plugin = self.fs / "opt/hmi/data/ro/Plugins/VehicleTelemetry"
        for path in [
            self.remote,
            self.kit / "payload",
            self.kit / "transport",
            self.plugin,
            self.fs / "etc/systemd/system/focussed.target.wants",
        ]:
            path.mkdir(parents=True)
        (self.base / "mount").write_text("ro")
        for name, data in [
            ("libVehicleTelemetry.so", b"new-dso"),
            ("VehicleTelemetry.manifest", b"manifest"),
            ("VehicleTelemetry.xml", b"resources"),
        ]:
            (self.kit / "payload" / name).write_bytes(data)
            (self.plugin / name).write_bytes(b"old-dso" if name == "libVehicleTelemetry.so" else data)
        (self.kit / "payload.sha256").write_text(
            "".join(sha(p) + "  " + p.name + "\n" for p in sorted((self.kit / "payload").iterdir()))
        )
        (self.kit / "transport/vehicle-telemetry-most").write_bytes(b"new-worker")
        (self.kit / "transport/vehicle-telemetry-most.service").write_bytes(b"new-unit")
        script = (ROOT / "install_most146.sh").read_text().replace(OLD, sha(self.plugin / "libVehicleTelemetry.so"))
        (self.remote / "install.sh").write_text(script)
        common = r"""
set -eu
KIT=KIT_PATH
FIRMWARE_ROOT=FS_PATH
PLUGIN=$FIRMWARE_ROOT/opt/hmi/data/ro/Plugins/VehicleTelemetry
PARENT=${PLUGIN%/*}
LOCK=BASE_PATH/lock
ID=fixture
regular() { [ -f "$1" ] && [ ! -L "$1" ]; }
die() { echo "STOP: $*"; exit 1; }
sha() { shasum -a 256 "$1" | awk '{print $1}'; }
verify() {
 while read -r expected name; do
  regular "$2/$name" && [ "$(sha "$2/$name")" = "$expected" ] || return 1
 done <"$1"
}
verify_kit() { :; }
firmware() { :; }
layout() { :; }
space() { :; }
owned() { :; }
mount_row() { echo fixture; }
root_ro() { [ "$(cat BASE_PATH/mount)" = ro ]; }
root_flags() { cat BASE_PATH/mount; }
signature() { echo fixture; }
remount_root() { printf '%s' "$1" > BASE_PATH/mount; }
restore_ro() { remount_root ro; }
loader() { :; }
boot_id() { echo fixture; }
process_start() { echo 1; }
lock_active() { [ -d "$LOCK" ]; }
pidof() { return 1; }
bounded() { shift; [ "$1" = sync ] || "$@"; }
copy_count=0
cp() { copy_count=$((copy_count+1)); [ "$copy_count" != "${TEST_FAIL_COPY:-0}" ] || return 77; command cp "$@"; }
"""
        import shlex

        common = (
            common.replace("KIT_PATH", shlex.quote(str(self.kit)))
            .replace("FS_PATH", shlex.quote(str(self.fs)))
            .replace("BASE_PATH", shlex.quote(str(self.base)))
        )
        (self.remote / "common.sh").write_text(common)

    def tearDown(self):
        self.tmp.cleanup()

    def run_install(self, action, fail=0, ok=True):
        import os

        r = subprocess.run(
            ["sh", self.remote / "install.sh", action],
            env=dict(os.environ, TEST_FAIL_COPY=str(fail)),
            capture_output=True,
            text=True,
        )
        self.assertEqual(r.returncode == 0, ok, r.stdout + r.stderr)
        self.assertEqual((self.base / "mount").read_text(), "ro", r.stdout + r.stderr)
        self.assertFalse((self.base / "lock").exists())
        return r

    def assert_old(self):
        self.assertEqual((self.plugin / "libVehicleTelemetry.so").read_bytes(), b"old-dso")
        self.assertEqual((self.plugin / "VehicleTelemetry.manifest").read_bytes(), b"manifest")
        self.assertFalse((self.fs / "opt/vehicle-telemetry").exists())
        self.assertFalse((self.fs / "etc/systemd/system/vehicle-telemetry-most.service").exists())
        self.assertFalse(
            (self.fs / "etc/systemd/system/focussed.target.wants/vehicle-telemetry-most.service").is_symlink()
        )

    def test_check_does_not_install(self):
        self.run_install("check")
        self.assert_old()

    def test_install_repeat_and_rollback(self):
        self.run_install("install")
        self.assertEqual((self.plugin / "libVehicleTelemetry.so").read_bytes(), b"new-dso")
        self.run_install("install")
        self.run_install("rollback")
        self.assert_old()

    def test_failed_helper_copy_restores_root_and_previous(self):
        self.run_install("install", fail=1, ok=False)
        self.assert_old()

    def test_failed_unit_copy_restores_root_and_previous(self):
        self.run_install("install", fail=2, ok=False)
        self.assert_old()

    def test_failed_plugin_copy_restores_root_and_previous(self):
        self.run_install("install", fail=3, ok=False)
        self.assert_old()

    def test_unknown_unit_is_preserved(self):
        unit = self.fs / "etc/systemd/system/vehicle-telemetry-most.service"
        unit.write_bytes(b"unrelated")
        self.run_install("install", ok=False)
        self.assertEqual(unit.read_bytes(), b"unrelated")
        self.assertEqual((self.plugin / "libVehicleTelemetry.so").read_bytes(), b"old-dso")


if __name__ == "__main__":
    unittest.main()
