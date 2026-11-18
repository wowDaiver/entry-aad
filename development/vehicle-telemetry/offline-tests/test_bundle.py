"""Full-bundle transactional upgrade rehearsal; temporary Mac filesystem only."""

import hashlib
import os
import shutil
import subprocess
import unittest
from pathlib import Path

from test_kit import ROOT, Fixture

OLD = Path(os.environ.get("VT_PREVIOUS_KIT", ROOT.parents[2] / "vehicle-telemetry-persistent-20261116"))


class BundleTests(Fixture):
    def setUp(self):
        super().setUp()
        self.remote_fixture()
        self.old = self.root / "old"
        self.old.mkdir()
        for line in (OLD / "REMOTE_SHA256SUMS").read_text().splitlines():
            _, name = line.split("  ", 1)
            target = self.old / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(OLD / name, target)
        shutil.copyfile(OLD / "REMOTE_SHA256SUMS", self.old / "REMOTE_SHA256SUMS")
        self.shell(["worker.sh", "install"])
        for p in (OLD / "payload").iterdir():
            shutil.copyfile(p, self.plugin / p.name)
        self.old_inodes = {p.name: p.stat().st_ino for p in self.plugin.iterdir()}
        self.fw = self.root / "iptables.rules"
        self.fw.write_bytes((self.kit / "network/iptables.rules.old").read_bytes())
        self.old_fw = self.fw.read_bytes()
        self.rule = self.root / "runtime-rule"
        self.ipt = self.root / "iptables"
        self.ipt.write_text(
            '#!/bin/sh\nset -eu\ncase "$1" in\n'
            '-C) test -f "' + str(self.rule) + '";;\n'
            '-I) test ! -f "' + str(self.root / "iptables-fail") + '"; touch "' + str(self.rule) + '";;\n'
            '-D) rm "' + str(self.rule) + '";;\n*) exit 90;;\nesac\n'
        )
        self.ipt.chmod(0o700)

    def run_bundle(self, action="upgrade", ok=True, network="vehicle-egs-hsfz-v1"):
        script = self.root / "bundle.sh"
        script.write_text(
            (ROOT / "bundle146.sh")
            .read_text()
            .replace("FW=/etc/sysconfig/iptables.rules", "FW=" + str(self.fw))
            .replace("IPT=/usr/sbin/iptables", "IPT=" + str(self.ipt))
        )
        r = subprocess.run(
            ["sh", str(script), str(self.kit), str(self.old), action, "persistent-startup-guard", network],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=20,
        )
        if ok:
            self.assertEqual(r.returncode, 0, r.stdout)
        else:
            self.assertNotEqual(r.returncode, 0, r.stdout)
        return r.stdout

    def assert_old_restored(self):
        for name, inode in self.old_inodes.items():
            target = self.plugin / (
                "VehicleTelemetry.registration-disabled" if name == "VehicleTelemetry.manifest" else name
            )
            self.assertEqual(target.stat().st_ino, inode)
            self.assertEqual(target.read_bytes(), (OLD / "payload" / name).read_bytes())
        self.assertEqual(self.fw.read_bytes(), self.old_fw)
        self.assertFalse(self.rule.exists())
        self.assertFalse((self.plugin / "VehicleTelemetry.manifest").exists())
        self.assertFalse((self.data / "persistent.enabled").exists())

    def test_check_does_not_mutate_root_or_files(self):
        before = (self.root / "remount-calls").read_text()
        self.assertIn("CHECK_PASS", self.run_bundle("check"))
        self.assertEqual((self.root / "remount-calls").read_text(), before)
        self.assertEqual(self.fw.read_bytes(), self.old_fw)
        self.assertFalse(self.rule.exists())
        for name, inode in self.old_inodes.items():
            self.assertEqual((self.plugin / name).stat().st_ino, inode)

    def test_full_upgrade_and_rollback_preserve_all_old_inodes(self):
        self.run_bundle()
        saved_inode = (self.plugin / "libVehicleTelemetry.so").stat().st_ino
        for line in (self.kit / "payload.sha256").read_text().splitlines():
            digest, name = line.split("  ", 1)
            self.assertEqual(hashlib.sha256((self.plugin / name).read_bytes()).hexdigest(), digest)
        self.assertTrue(self.rule.exists())
        self.assertTrue(self.flags.read_text().startswith("ro,"))
        self.run_bundle("rollback")
        self.assert_old_restored()
        self.assertEqual(
            next(self.parent.glob(".VehicleTelemetry.after-rollback-*"))
            .joinpath("libVehicleTelemetry.so")
            .stat()
            .st_ino,
            saved_inode,
        )
        self.assertFalse(self.lock.exists())
        self.assertTrue(self.flags.read_text().startswith("ro,"))

    def test_firewall_failure_automatically_restores_entire_bundle(self):
        (self.root / "iptables-fail").touch()
        self.run_bundle(ok=False)
        self.assert_old_restored()
        self.assertTrue(self.flags.read_text().startswith("ro,"))
        self.assertFalse(self.lock.exists())

    def test_copy_failure_never_publishes_a_partial_manifest(self):
        common = self.kit / "remote/common.sh"
        common.write_text(
            common.read_text()
            + '\ncp() { case "$1" in *VehicleTelemetry_Texts.zip) return 1;; esac; command cp "$@"; }\n'
        )
        self.refresh_remote()
        self.run_bundle(ok=False)
        self.assert_old_restored()
        stage = next(self.parent.glob(".VehicleTelemetry.stage-sources-*"))
        self.assertFalse((stage / "VehicleTelemetry.manifest").exists())
        self.assertTrue(self.flags.read_text().startswith("ro,"))
        self.assertFalse(self.lock.exists())

    def test_unknown_bundle_is_preserved_before_remount(self):
        (self.plugin / "VehicleTelemetry.xml").write_bytes(b"foreign resources")
        before = (self.root / "remount-calls").read_text()
        self.run_bundle(ok=False)
        self.assertEqual((self.root / "remount-calls").read_text(), before)
        self.assertEqual((self.plugin / "VehicleTelemetry.xml").read_bytes(), b"foreign resources")

    def test_unknown_firewall_is_preserved_before_remount(self):
        self.fw.write_bytes(b"foreign config")
        before = (self.root / "remount-calls").read_text()
        self.run_bundle(ok=False)
        self.assertEqual((self.root / "remount-calls").read_text(), before)
        self.assertEqual(self.fw.read_bytes(), b"foreign config")

    def test_failed_ro_restore_keeps_lock_and_disables_new_registration(self):
        (self.root / "ro-fail").touch()
        self.run_bundle(ok=False)
        self.assert_old_restored()
        self.assertTrue(self.lock.exists())
        self.assertTrue(self.flags.read_text().startswith("rw,"))

    def test_rollback_works_even_if_new_loader_or_android_auto_is_unavailable(self):
        self.run_bundle()
        (self.root / "loader-fail").touch()
        (self.root / "aa-running").touch()
        self.run_bundle("rollback")
        self.assert_old_restored()
        self.assertFalse(self.lock.exists())

    def test_graphics_upgrade_and_rollback_preserve_existing_network(self):
        self.fw.write_bytes((self.kit / "network/iptables.rules.new").read_bytes())
        self.rule.touch()
        before = self.fw.read_bytes()
        inode = self.fw.stat().st_ino
        self.run_bundle("check", network="none")
        self.run_bundle(network="none")
        self.assertEqual(self.fw.read_bytes(), before)
        self.assertEqual(self.fw.stat().st_ino, inode)
        self.run_bundle("rollback", network="none")
        self.assertEqual(self.fw.read_bytes(), before)
        self.assertEqual(self.fw.stat().st_ino, inode)
        self.assertTrue(self.rule.exists())
        self.assertFalse(self.lock.exists())
        for name, old_inode in self.old_inodes.items():
            path = self.plugin / (
                "VehicleTelemetry.registration-disabled" if name == "VehicleTelemetry.manifest" else name
            )
            self.assertEqual(path.stat().st_ino, old_inode)

    def test_graphics_failure_restores_bundle_without_touching_network(self):
        self.fw.write_bytes((self.kit / "network/iptables.rules.new").read_bytes())
        self.rule.touch()
        before = self.fw.read_bytes()
        common = self.kit / "remote/common.sh"
        common.write_text(
            common.read_text()
            + '\ncp() { case "$1" in *VehicleTelemetry_Texts.zip) return 1;; esac; command cp "$@"; }\n'
        )
        self.refresh_remote()
        self.run_bundle(ok=False, network="none")
        self.assertEqual(self.fw.read_bytes(), before)
        self.assertTrue(self.rule.exists())
        self.assertFalse((self.plugin / "VehicleTelemetry.manifest").exists())
        self.assertEqual(
            (self.plugin / "libVehicleTelemetry.so").stat().st_ino, self.old_inodes["libVehicleTelemetry.so"]
        )
        self.assertTrue(self.flags.read_text().startswith("ro,"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
