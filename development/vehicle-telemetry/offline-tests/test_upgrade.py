"""Rehearse the DSO-only update and inode-preserving rollback locally."""

import hashlib
import os
import shutil
import subprocess
import unittest
from pathlib import Path

from test_kit import ROOT, Fixture

OLD = Path(os.environ.get("VT_PREVIOUS_KIT", ROOT.parents[2] / "vehicle-telemetry-continuous-20261116"))


class UpgradeTests(Fixture):
    def setUp(self):
        super().setUp()
        self.remote_fixture()
        self.old = self.root / "previous-kit"
        self.old.mkdir()
        # Only the immutable transport files; no real credentials or reports.
        for line in (OLD / "REMOTE_SHA256SUMS").read_text().splitlines():
            _, name = line.split("  ", 1)
            target = self.old / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(OLD / name, target)
        shutil.copyfile(OLD / "REMOTE_SHA256SUMS", self.old / "REMOTE_SHA256SUMS")
        self.shell(["worker.sh", "install"])
        shutil.copyfile(OLD / "payload/libVehicleTelemetry.so", self.plugin / "libVehicleTelemetry.so")
        (self.data / "persistent.enabled").unlink()
        (self.data / "trial-consumed").write_text("vehicle-telemetry-146-single-start-v1\n")
        (self.data / "trial.log").write_text("previous init and samples\n")
        self.old_inode = (self.plugin / "libVehicleTelemetry.so").stat().st_ino

    def upgrade(self, action="upgrade", check=True, policy="persistent-startup-guard", network="none"):
        script = ROOT / "upgrade146.sh"
        if hasattr(self, "fw"):
            script = self.root / "upgrader-test.sh"
            script.write_text(
                (ROOT / "upgrade146.sh")
                .read_text()
                .replace("FW=/etc/sysconfig/iptables.rules", "FW=" + str(self.fw))
                .replace("IPT=/usr/sbin/iptables", "IPT=" + str(self.ipt))
            )
        r = subprocess.run(
            ["sh", str(script), str(self.kit), str(self.old), action, policy, network],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=20,
        )
        if check:
            self.assertEqual(r.returncode, 0, r.stdout)
        return r

    def prepare_egs(self):
        if (
            hashlib.sha256((OLD / "payload/libVehicleTelemetry.so").read_bytes()).hexdigest()
            != "f8eb7a0e6b416693fb6f83322be66ff0a7191a713ca0d4932acb84189c1b7157"
        ):
            self.skipTest("EGS network upgrade requires the verified persistent predecessor")
        evidence = ROOT.parents[1] / "research/evidence/egs-source-2026-11-16"
        self.fw = self.root / "iptables.rules"
        self.fw.write_bytes((evidence / "iptables.rules.before").read_bytes())
        self.original_fw = self.fw.read_bytes()
        self.expected_fw = (evidence / "iptables.rules.egs-v1").read_bytes()
        self.ipt = self.root / "iptables"
        self.runtime_rule = self.root / "runtime-rule"
        self.ipt_failure = self.root / "iptables-fail"
        self.ipt.write_text(
            '#!/bin/sh\nset -eu\ncase "$1" in\n-C) test -f "'
            + str(self.runtime_rule)
            + '";;\n-I) test ! -f "'
            + str(self.ipt_failure)
            + '"; touch "'
            + str(self.runtime_rule)
            + '";;\n-D) rm "'
            + str(self.runtime_rule)
            + '";;\n*) exit 90;;\nesac\n'
        )
        self.ipt.chmod(0o700)

    def test_egs_config_and_runtime_rule_are_both_rolled_back(self):
        self.prepare_egs()
        before = (self.root / "remount-calls").read_text()
        self.upgrade("check", network="vehicle-egs-hsfz-v1")
        self.assertEqual((self.root / "remount-calls").read_text(), before)
        self.assertEqual(self.fw.read_bytes(), self.original_fw)
        self.assertFalse(self.runtime_rule.exists())
        self.upgrade(network="vehicle-egs-hsfz-v1")
        self.assertEqual(self.fw.read_bytes(), self.expected_fw)
        self.assertTrue(self.runtime_rule.exists())
        self.assertEqual((self.data / "iptables.rules.before-egs-v1").read_bytes(), self.original_fw)
        self.upgrade("rollback", network="vehicle-egs-hsfz-v1")
        self.assertEqual(self.fw.read_bytes(), self.original_fw)
        self.assertFalse(self.runtime_rule.exists())
        self.assertEqual((self.plugin / "libVehicleTelemetry.so").stat().st_ino, self.old_inode)
        self.assertTrue(self.flags.read_text().startswith("ro,"))
        self.assertFalse(self.lock.exists())

    def test_egs_rule_failure_restores_configuration_and_old_plugin(self):
        self.prepare_egs()
        self.ipt_failure.touch()
        self.assertNotEqual(self.upgrade(network="vehicle-egs-hsfz-v1", check=False).returncode, 0)
        self.assertEqual(self.fw.read_bytes(), self.original_fw)
        self.assertFalse(self.runtime_rule.exists())
        self.assertEqual((self.plugin / "libVehicleTelemetry.so").stat().st_ino, self.old_inode)
        self.assertFalse((self.plugin / "VehicleTelemetry.manifest").exists())
        self.assertTrue(self.flags.read_text().startswith("ro,"))
        self.assertFalse(self.lock.exists())

    def test_egs_unknown_configuration_is_preserved_before_remount(self):
        self.prepare_egs()
        self.fw.write_bytes(b"foreign configuration")
        before = (self.root / "remount-calls").read_text()
        self.assertNotEqual(self.upgrade(network="vehicle-egs-hsfz-v1", check=False).returncode, 0)
        self.assertEqual(self.fw.read_bytes(), b"foreign configuration")
        self.assertEqual((self.root / "remount-calls").read_text(), before)

    def test_running_predecessor_can_be_updated_but_running_rolled_back_candidate_cannot(self):
        (self.data / "persistent.state").write_text("VT146P1 fixture-boot " + str(os.getpid()) + " 0 1 0\n")
        self.upgrade()
        self.upgrade("rollback")
        (self.data / "persistent.state").write_text("VT146P1 fixture-boot " + str(os.getpid()) + " 0 1 0\n")
        self.assertIn("RESTART_AFTER_PREVIOUS_ROLLBACK_FIRST", self.upgrade(check=False).stdout)

    def test_upgrade_and_rollback_preserve_both_mapped_inodes(self):
        self.upgrade("check")
        self.upgrade()
        backups = list(self.parent.glob(".VehicleTelemetry.rollback-*.so"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].stat().st_ino, self.old_inode)
        self.assertEqual(
            (self.plugin / "libVehicleTelemetry.so").read_bytes(),
            (self.kit / "payload/libVehicleTelemetry.so").read_bytes(),
        )
        new_inode = (self.plugin / "libVehicleTelemetry.so").stat().st_ino
        self.assertNotEqual(new_inode, self.old_inode)
        self.assertTrue((self.data / "persistent.enabled").is_file())
        self.assertFalse((self.data / "trial-consumed").exists())
        self.assertFalse((self.data / "trial-arm").exists())
        self.assertTrue(list(self.data.glob("trial-consumed.saved-*")))
        self.assertTrue(list(self.data.glob("trial.log.saved-*")))
        self.assertTrue(self.flags.read_text().startswith("ro,"))
        self.assertFalse(self.lock.exists())
        self.upgrade()  # idempotent, does not rearm a used candidate
        self.upgrade("rollback")
        self.assertEqual((self.plugin / "libVehicleTelemetry.so").stat().st_ino, self.old_inode)
        self.assertEqual(list(self.parent.glob(".VehicleTelemetry.continuous-*.so"))[0].stat().st_ino, new_inode)
        self.assertFalse((self.plugin / "VehicleTelemetry.manifest").exists())
        self.assertFalse((self.data / "trial-arm").exists())
        self.assertFalse((self.data / "persistent.enabled").exists())
        self.assertTrue(self.flags.read_text().startswith("ro,"))
        self.assertFalse(self.lock.exists())

    def test_unknown_dso_blocks_before_remount(self):
        (self.plugin / "libVehicleTelemetry.so").write_bytes(b"foreign")
        before = (self.root / "remount-calls").read_text()
        self.assertNotEqual(self.upgrade(check=False).returncode, 0)
        self.assertEqual((self.root / "remount-calls").read_text(), before)

    def test_idempotent_update_never_resets_a_blocked_persistent_guard(self):
        self.upgrade()
        (self.data / "persistent.disabled").write_text("native-error\n")
        self.upgrade()
        self.assertEqual((self.data / "persistent.disabled").read_text(), "native-error\n")

    def test_legacy_single_start_update_policy_remains_explicit(self):
        self.upgrade(policy="single-start")
        self.assertTrue((self.data / "trial-arm").exists())
        self.assertFalse((self.data / "persistent.enabled").exists())

    def test_failed_new_readback_restores_old_and_disables_registration(self):
        common = self.kit / "remote/common.sh"
        common.write_text(common.read_text() + "\nresources_valid() { return 1; }\n")
        self.refresh_remote()
        self.assertNotEqual(self.upgrade(check=False).returncode, 0)
        self.assertEqual((self.plugin / "libVehicleTelemetry.so").stat().st_ino, self.old_inode)
        self.assertFalse((self.plugin / "VehicleTelemetry.manifest").exists())
        self.assertTrue(self.flags.read_text().startswith("ro,"))
        self.assertFalse(self.lock.exists())

    def test_ro_failure_restores_old_then_existing_disable_can_restore_ro(self):
        (self.root / "ro-fail").touch()
        self.assertNotEqual(self.upgrade(check=False).returncode, 0)
        self.assertEqual((self.plugin / "libVehicleTelemetry.so").stat().st_ino, self.old_inode)
        self.assertFalse((self.plugin / "VehicleTelemetry.manifest").exists())
        (self.root / "ro-fail").unlink()
        self.shell(["worker.sh", "disable"])
        self.assertTrue(self.flags.read_text().startswith("ro,"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
