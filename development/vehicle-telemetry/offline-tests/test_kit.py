"""Offline installation/recovery rehearsals. Never mounts or connects to a HU."""

import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KIT = Path(os.environ.get("VT_TEST_KIT", ROOT.parents[2] / "vehicle-telemetry-offline"))
sys.path.insert(0, str(KIT))
spec = importlib.util.spec_from_file_location("vt_host", KIT / "host.py")
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def sums(base, paths, output):
    output.write_text(
        "".join(hashlib.sha256((base / p).read_bytes()).hexdigest() + "  " + p + "\n" for p in sorted(paths))
    )


class Fixture(unittest.TestCase):
    def setUp(self):
        # /private/tmp has no /var symlink ancestor; all target paths stay inside it.
        self.temp = tempfile.TemporaryDirectory(prefix="vt-offline-test-", dir="/private/tmp")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.kit = self.root / "kit"
        self.kit.mkdir()
        for filename in host.REQUIRED:
            target = self.kit / filename
            target.parent.mkdir(parents=True, exist_ok=True)
            if filename == "hu-password.txt":
                target.write_text("fixture-password\n")
            elif filename == "hu-known_hosts":
                target.write_text("entrynav2-hu ssh-ed25519 fixture-key\n")
            else:
                shutil.copyfile(KIT / filename, target)
        sums(self.kit, host.REQUIRED, self.kit / "SHA256SUMS")

    def shell(self, action, check=True):
        result = subprocess.run(
            ["sh", str(self.kit / "remote" / action[0]), *action[1:]],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=20,
        )
        if check:
            self.assertEqual(result.returncode, 0, result.stdout)
        return result

    def remote_fixture(self):
        self.parent = self.root / "hu/Plugins"
        self.parent.mkdir(parents=True)
        self.plugin = self.parent / "VehicleTelemetry"
        self.data = self.root / "hu/journal"
        self.lock = self.root / "hu/lock"
        self.flags = self.root / "flags"
        self.flags.write_text("ro,relatime,data=ordered\n")
        self.sig = self.root / "signature"
        self.sig.write_text("/dev/fixture|ext4\n1:2\n")
        self.firmware = self.root / "firmware"
        self.firmware.mkdir()
        (self.firmware / "hmi").write_bytes(b"\x7fELFfixture146")
        sums(self.firmware, ["hmi"], self.kit / "firmware.sha256")
        common = self.kit / "remote/common.sh"
        text = common.read_text()
        for old, new in {
            "PARENT=/opt/hmi/data/ro/Plugins": "PARENT=" + str(self.parent),
            "DATA=/var/local/data/VehicleTelemetry146": "DATA=" + str(self.data),
            "LOCK=/tmp/vehicle-telemetry-146-v1.lock": "LOCK=" + str(self.lock),
            "FIRMWARE_ROOT=/\n": "FIRMWARE_ROOT=" + str(self.firmware) + "\n",
        }.items():
            self.assertIn(old, text)
            text = text.replace(old, new)
        # Production bundle contains none of these overrides. No Mac mount, ARM loader or sync is called.
        text += f"""
layout() {{ journal_valid; }}
space() {{ :; }}
root_flags() {{ cat '{self.flags}'; }}
signature() {{ cat '{self.sig}'; }}
boot_id() {{ echo fixture-boot; }}
process_start() {{ kill -0 "$1" 2>/dev/null && echo fixture-start; }}
bounded() {{ shift; if [ "$1" = sync ]; then return 0; fi; "$@"; }}
pidof() {{ if [ -f '{self.root}/aa-running' ] && [ "$1" = entry-aad ]; then echo 123; return 0; fi; return 1; }}
loader() {{ [ ! -f '{self.root}/loader-fail' ] || die LOADER_FAILED; echo FIXTURE_LOADER_PASS; }}
remount_root() {{
    echo "$1" >>'{self.root}/remount-calls'
    case "$1" in rw) echo rw,relatime,data=ordered >'{self.flags}';;
    *) [ ! -f '{self.root}/ro-fail' ] || return 1; echo "$1" >'{self.flags}';; esac
}}
"""
        common.write_text(text)
        self.refresh_remote()

    def refresh_remote(self):
        paths = [p for p in host.REMOTE_FILES if p != "REMOTE_SHA256SUMS"]
        sums(self.kit, paths, self.kit / "REMOTE_SHA256SUMS")


class LocalTests(Fixture):
    def test_local_check_does_not_open_ssh(self):
        fakebin = self.root / "bin"
        fakebin.mkdir()
        ssh = fakebin / "ssh"
        ssh.write_text('#!/bin/sh\ntouch "' + str(self.root / "SSH_CALLED") + '"\nexit 99\n')
        ssh.chmod(0o700)
        result = subprocess.run(
            ["sh", str(self.kit / "00-local-check.sh")],
            env={**os.environ, "PATH": str(fakebin) + ":" + os.environ["PATH"]},
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("LOCAL_OFFLINE_KIT_PASS", result.stdout)
        self.assertFalse((self.root / "SSH_CALLED").exists())

    def test_corruption_and_missing_file_block_before_connection(self):
        for change in ("corrupt", "missing"):
            original = (self.kit / "payload/VehicleTelemetry.xml").read_bytes()
            if change == "corrupt":
                (self.kit / "payload/VehicleTelemetry.xml").write_bytes(b"changed")
            else:
                (self.kit / "payload/VehicleTelemetry.xml").unlink()
            before = host.HERE
            host.HERE = self.kit
            try:
                with self.assertRaises(RuntimeError):
                    host.local_check()
            finally:
                host.HERE = before
                (self.kit / "payload/VehicleTelemetry.xml").write_bytes(original)

    def test_payload_link_is_rejected(self):
        target = self.kit / "payload/VehicleTelemetry.xml"
        original = self.root / "original.xml"
        target.rename(original)
        target.symlink_to(original)
        before = host.HERE
        host.HERE = self.kit
        try:
            with self.assertRaises(RuntimeError):
                host.local_check()
        finally:
            host.HERE = before


class RemoteTests(Fixture):
    def setUp(self):
        super().setUp()
        self.remote_fixture()

    def test_guard_status_works_without_busybox_head_c(self):
        self.shell(["worker.sh", "install"])
        (self.data / "persistent.state").write_text("VT146P1 00000000-0000-0000-0000-000000000001 100 0 1 0\n")
        common = self.kit / "remote/common.sh"
        common.write_text(common.read_text() + "\nhead() { return 1; }\n")
        self.refresh_remote()
        self.assertIn("START_GUARD_STATE=VT146P1", self.shell(["control.sh", "status"]).stdout)

    def test_install_disable_and_reenable_preserve_dso_inode(self):
        self.shell(["control.sh", "check"])
        self.assertFalse(self.plugin.exists())
        self.shell(["worker.sh", "install"])
        inode = (self.plugin / "libVehicleTelemetry.so").stat().st_ino
        self.assertTrue((self.plugin / "VehicleTelemetry.manifest").is_file())
        self.assertTrue(self.flags.read_text().startswith("ro,"))
        self.assertFalse(self.lock.exists())
        self.shell(["worker.sh", "install"])
        self.shell(["worker.sh", "disable"])
        self.shell(["worker.sh", "disable"])
        self.assertFalse((self.plugin / "VehicleTelemetry.manifest").exists())
        self.assertEqual((self.plugin / "libVehicleTelemetry.so").stat().st_ino, inode)
        self.shell(["worker.sh", "install"])
        self.assertEqual((self.plugin / "libVehicleTelemetry.so").stat().st_ino, inode)
        self.assertIn("FILES=installed", self.shell(["control.sh", "status"]).stdout)

    def test_wrong_firmware_prevents_root_write(self):
        (self.firmware / "hmi").write_bytes(b"other firmware")
        result = self.shell(["worker.sh", "install"], check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.plugin.exists())
        self.assertFalse((self.root / "remount-calls").exists())

    def test_mount_options_exact_match_independent_of_broken_busybox_grep_x(self):
        fakebin = self.root / "bin"
        fakebin.mkdir()
        grep = fakebin / "grep"
        grep.write_text("#!/bin/sh\nexit 1\n")
        grep.chmod(0o700)
        command = '. "$1"; root_ro'
        for flags, wanted in [
            ("ro,relatime,data=ordered", 0),
            ("rw,relatime", 1),
            ("rw,errors=remount-ro", 1),
            ("relatime,ro", 0),
        ]:
            self.flags.write_text(flags + "\n")
            result = subprocess.run(
                ["sh", "-c", command, "fixture", str(self.kit / "remote/common.sh")],
                env={**os.environ, "PATH": str(fakebin) + ":" + os.environ["PATH"]},
                capture_output=True,
            )
            self.assertEqual(result.returncode, wanted, result.stderr)

    def test_failed_persistent_guard_cannot_be_reset_by_reinstall(self):
        self.shell(["worker.sh", "install"])
        self.assertEqual((self.data / "persistent.enabled").read_text(), "vehicle-telemetry-146-persistent-v1\n")
        self.shell(["worker.sh", "disable"])
        self.assertFalse((self.data / "persistent.enabled").exists())
        (self.data / "persistent.disabled").write_text("native-error\n")
        before = (self.root / "remount-calls").read_text()
        self.assertNotEqual(self.shell(["worker.sh", "install"], check=False).returncode, 0)
        self.assertFalse((self.plugin / "VehicleTelemetry.manifest").exists())
        self.assertEqual((self.root / "remount-calls").read_text(), before)

    def test_unknown_plugin_never_overwritten(self):
        self.plugin.mkdir()
        sentinel = self.plugin / "foreign.so"
        sentinel.write_bytes(b"keep")
        for action in ("install", "disable"):
            self.assertNotEqual(self.shell(["worker.sh", action], check=False).returncode, 0)
        self.assertEqual(sentinel.read_bytes(), b"keep")
        self.assertFalse((self.root / "remount-calls").exists())

    def test_active_aa_or_loader_failure_prevents_install(self):
        for flag in ("aa-running", "loader-fail"):
            (self.root / flag).touch()
            self.assertNotEqual(self.shell(["worker.sh", "install"], check=False).returncode, 0)
            self.assertFalse(self.plugin.exists())
            self.assertFalse((self.root / "remount-calls").exists())
            self.assertFalse(self.lock.exists())
            (self.root / flag).unlink()

    def test_changed_manifest_preserved(self):
        self.shell(["worker.sh", "install"])
        prior_mounts = (self.root / "remount-calls").read_text()
        manifest = self.plugin / "VehicleTelemetry.manifest"
        manifest.write_bytes(b"foreign update")
        self.assertNotEqual(self.shell(["worker.sh", "disable"], check=False).returncode, 0)
        self.assertEqual(manifest.read_bytes(), b"foreign update")
        self.assertTrue(self.flags.read_text().startswith("ro,"))
        self.assertEqual((self.root / "remount-calls").read_text(), prior_mounts)

    def test_worker_finishes_after_host_connection_is_lost(self):
        common = self.kit / "remote/common.sh"
        common.write_text(common.read_text() + "\nloader() { sleep 1; echo FIXTURE_LOADER_PASS; }\n")
        self.refresh_remote()

        class LostConnection:
            calls = 0

            def run(self, command):
                self.calls += 1
                if self.calls > 1:
                    raise OSError("simulated ENET disconnect")
                return subprocess.run(
                    ["sh", "-c", command],
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                    timeout=10,
                )

        reports = self.root / "reports"
        reports.mkdir()
        with self.assertRaises(OSError):
            host.worker(LostConnection(), str(self.kit), "install", reports)
        deadline = time.monotonic() + 10
        while not (self.kit / "exit").exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        self.assertEqual((self.kit / "exit").read_text().strip(), "0")
        self.assertTrue((self.plugin / "VehicleTelemetry.manifest").is_file())
        self.assertTrue(self.flags.read_text().startswith("ro,"))
        self.assertFalse(self.lock.exists())

    def test_ro_failure_disables_registration_and_can_recover(self):
        (self.root / "ro-fail").touch()
        self.assertNotEqual(self.shell(["worker.sh", "install"], check=False).returncode, 0)
        self.assertFalse((self.plugin / "VehicleTelemetry.manifest").exists())
        self.assertTrue((self.plugin / "libVehicleTelemetry.so").is_file())
        self.assertTrue(self.flags.read_text().startswith("rw,"))
        (self.root / "ro-fail").unlink()
        self.shell(["worker.sh", "disable"])
        self.assertTrue(self.flags.read_text().startswith("ro,"))

    def test_interrupted_before_plugin_publication_can_restore_ro(self):
        self.data.mkdir()
        (self.data / "owner").write_text(host.ID + "\n")
        (self.data / "root-signature").write_text(self.sig.read_text())
        (self.data / "root-flags").write_text(self.flags.read_text())
        self.flags.write_text("rw,relatime,data=ordered\n")
        self.shell(["worker.sh", "disable"])
        self.assertTrue(self.flags.read_text().startswith("ro,"))
        self.assertFalse(self.plugin.exists())

    def test_stale_lock_preserved_live_lock_blocked(self):
        self.lock.mkdir()
        for name, value in {
            "owner": host.ID,
            "pid": str(os.getpid()),
            "boot": "fixture-boot",
            "start": "fixture-start",
        }.items():
            (self.lock / name).write_text(value + "\n")
        self.assertNotEqual(self.shell(["control.sh", "unlock"], check=False).returncode, 0)
        self.assertTrue(self.lock.is_dir())
        (self.lock / "boot").write_text("previous-boot\n")
        self.shell(["control.sh", "unlock"])
        self.assertFalse(self.lock.exists())
        self.assertEqual(len(list(self.lock.parent.glob("lock.saved-*"))), 1)

    def test_journal_symlink_blocks_operation(self):
        self.data.mkdir()
        (self.data / "owner").write_text(host.ID + "\n")
        sentinel = self.root / "sentinel"
        sentinel.write_text("preserve")
        (self.data / "root-flags").symlink_to(sentinel)
        self.assertNotEqual(self.shell(["worker.sh", "install"], check=False).returncode, 0)
        self.assertEqual(sentinel.read_text(), "preserve")
        self.assertFalse((self.root / "remount-calls").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
