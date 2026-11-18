"""End-to-end saved wire response -> native Model -> RHMI/panel -> PNG checks."""

import copy
import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("offline_mapping", ROOT / "dashboard/offline_mapping.py")
mapping = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mapping)


class SourceMappingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        mapping.build()

    def run_replay(self, events, age=0):
        with tempfile.TemporaryDirectory() as tmp:
            png = Path(tmp) / "panel.png"
            report = mapping.replay(events, png, age)
            self.assertTrue(png.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"))
            self.assertEqual(struct.unpack(">II", png.read_bytes()[16:24]), (1200, 424))
            return report

    def from_capture(self, name):
        return self.run_replay(mapping.capture_events(mapping.EVIDENCE / name)[0])

    @staticmethod
    def metrics(report):
        return {r["name"]: r for r in report["metrics"]}

    def test_recorded_dde_values_reach_electrical_fields_and_oil_text_page(self):
        report = self.from_capture("d83-source/live-d83-20261117.json")
        metrics = self.metrics(report)
        self.assertAlmostEqual(report["panel"]["voltage"]["value"], 14.817)
        self.assertEqual(metrics["voltage"]["hmi_model"], 109)
        self.assertEqual(metrics["voltage"]["hmi_state"], 13)
        self.assertAlmostEqual(metrics["oil_pressure"]["value"], 1.287)
        self.assertEqual((metrics["oil_pressure"]["hmi_model"], metrics["oil_pressure"]["hmi_state"]), (130, 11))
        self.assertEqual(metrics["battery_temperature"]["value"], 8)
        self.assertIsNone(report["panel"]["soc"]["value"])
        for field in ("torque", "power", "brake", "recuperation"):
            self.assertIsNone(report["panel"][field]["value"])

    def test_recorded_signed_current_is_not_used_as_recuperation(self):
        report = self.from_capture("d83-source/live-current-20261117.json")
        self.assertAlmostEqual(report["panel"]["current"]["value"], -4.6)
        self.assertIsNone(report["panel"]["recuperation"]["value"])

    def test_recorded_brake_states_and_full_igr_record_reach_native_model(self):
        for name, state in (("parked-brake-engaged.json", 1), ("parked-brake-released.json", 0)):
            path = mapping.PROJECT / "tests/fixtures/vehicle-telemetry/brake-regen-2026-11-18" / name
            report = self.run_replay(mapping.capture_events(path)[0])
            self.assertEqual(report["panel"]["brake"]["value"], state)
            self.assertAlmostEqual(self.metrics(report)["recuperated_charge"]["value"], 5718681.2)
            self.assertIsNone(report["panel"]["recuperation"]["value"])  # one snapshot cannot indicate activity
            mapping.verify_ui_bindings(report)

    def test_recorded_followup_soc_and_torque_reach_panel_without_invented_rpm(self):
        capture = (
            mapping.PROJECT / "tests/fixtures/vehicle-telemetry/panel-followup-2026-11-18/missing-sources-live.json"
        )
        report = self.run_replay(mapping.capture_events(capture)[0])
        mapping.verify_ui_bindings(report)
        self.assertEqual(report["panel"]["soc"]["value"], 87)
        self.assertAlmostEqual(report["panel"]["torque"]["value"], 227.51191578)
        self.assertEqual(report["panel"]["mode"]["value"], 3)
        self.assertIsNone(report["panel"]["power"]["value"])
        self.assertEqual(self.metrics(report)["gear_lever"]["value"], 0)

    def test_recorded_egs_p_and_temperature_reach_panel_without_invented_brake(self):
        report = self.from_capture("panel-live-20261117-1602.json")
        metrics = self.metrics(report)
        self.assertEqual(report["panel"]["atf"]["value"], 34)
        self.assertEqual(metrics["gear_mode"]["enum_text"], "P")
        self.assertEqual(metrics["gear_selector"]["enum_text"], "P")
        self.assertEqual(metrics["converter_lock"]["enum_text"], "Открыта")
        self.assertEqual(metrics["converter_rpm"]["value"], 753)
        self.assertEqual(metrics["output_shaft_rpm"]["value"], 0)
        self.assertIsNone(report["panel"]["brake"]["value"])
        self.assertIsNone(metrics["oil_pressure"]["value"])  # 586F C8 is not oil.

    def test_egs_native_engine_rpm_and_gear_code_do_not_replace_main_sources(self):
        report = self.from_capture("panel-followup-20261117-1610.json")
        metrics = self.metrics(report)
        self.assertEqual(metrics["turbine_rpm"]["value"], 736)
        self.assertEqual(metrics["egs_engine_rpm"]["value"], 768)
        self.assertEqual(metrics["transmission_gear_code"]["enum_text"], "1")
        self.assertIsNone(report["panel"]["rpm"]["value"])
        self.assertIsNone(report["panel"]["mode"]["value"])

    def test_obd_wire_4104_maps_to_load_without_using_rejected_f404(self):
        report = self.from_capture("d83-source/engine-load-check.json")
        self.assertAlmostEqual(report["panel"]["load"]["value"], 133 * 100 / 255)
        self.assertEqual(self.metrics(report)["engine_load"]["source"], "obd01")
        self.assertIsNone(report["panel"]["speed"]["value"])  # no implicit 010D fallback

    def test_cds_zero_speed_temperature_rpm_and_soc_follow_same_ui_map(self):
        report = self.run_replay(
            'C 0 {"temperature":{"engine":88,"oil":101}}\n'
            'C 1 {"RPMSpeed":1234}\nC 2 {"speedActual":0}\n'
            'C 3 {"averageConsumption":{"averageConsumption1":6.7,"unit":1}}\nC 4 {"battery":73}\n'
        )
        self.assertEqual(
            [report["panel"][f]["value"] for f in ("temperature", "oil", "rpm", "speed", "soc")], [88, 101, 1234, 0, 73]
        )
        self.assertEqual(report["panel"]["speed"]["status"], "valid")
        self.assertAlmostEqual(self.metrics(report)["average_consumption"]["value"], 6.7)
        mapping.verify_ui_bindings(report)

    def test_foreign_missing_identity_wrong_ecu_and_short_records_are_rejected(self):
        for events in (
            "D 12 5866 39e1\n",
            "I 12 0f1234\nD 12 5866 39e1\n",
            "I 18 0f21d0\nD 18 5866 39e1\n",
            "I 12 0f1ff0\nD 12 5866 39\n",
        ):
            with self.subTest(events=events):
                report = self.run_replay(events)
                self.assertIsNone(report["panel"]["voltage"]["value"])
                self.assertFalse(report["events"][-1]["accepted"])

    def test_record_failure_preserves_last_current_and_leaves_other_fields_valid(self):
        report = self.run_replay("I 12 0f1ff0\nD 12 5866 39e1\nD 12 43c0 7fa3\nN 12 43c0\n")
        self.assertEqual(report["panel"]["current"]["status"], "stale")
        self.assertAlmostEqual(report["panel"]["current"]["value"], -4.6)
        self.assertEqual(report["panel"]["voltage"]["status"], "valid")
        self.assertEqual(self.metrics(report)["battery_current"]["received_at_replay"], 0)

    def test_old_replay_clock_hides_expired_panel_values_and_bindings_match_xml(self):
        events = mapping.capture_events(mapping.CAPTURES[2])[0]
        report = self.run_replay(events, 61)
        self.assertTrue(all(r["value"] is None for r in report["panel"].values()))
        mapping.verify_ui_bindings(report)
        self.assertEqual(len(report["metrics"]), 31)

    def test_modified_capture_interpretation_is_ignored_but_raw_mismatch_fails(self):
        capture = json.loads(mapping.CAPTURES[2].read_text())
        row = next(r for r in capture["results"] if r["did"] == "0x5866")
        row["interpretation"] = {"value": 9999}
        self.assertEqual(mapping.wire_record(row)[3], bytes.fromhex("39e1"))
        bad = copy.deepcopy(row)
        bad["data_hex"] = "0000"
        with self.assertRaises(ValueError):
            mapping.wire_record(bad)
        bad = copy.deepcopy(row)
        bad["frames"][-1] = bad["frames"][-1].replace("12f462", "18f462")
        with self.assertRaises(ValueError):
            mapping.wire_record(bad)


if __name__ == "__main__":
    unittest.main()
