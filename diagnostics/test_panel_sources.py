import contextlib
import io
import struct
import unittest
from unittest.mock import patch

import read_panel_sources as panel


def frame(ecu, uds, control=1):
    body = bytes((ecu, 0xF4)) + uds
    return struct.pack("!IH", len(body), control) + body


class Wire:
    def __init__(self, data):
        self.data = data
        self.sent = []

    def settimeout(self, timeout):
        assert 0 < timeout <= 3

    def sendall(self, data):
        self.sent.append(data)

    def recv(self, size):
        size = min(size, 2)  # fragmented headers and bodies
        chunk, self.data = self.data[:size], self.data[size:]
        return chunk


class PanelSourcesTests(unittest.TestCase):
    def test_brake_regen_plans_and_candidate_semantics(self):
        self.assertEqual(sum(map(len, panel.BRAKE_REGEN.values())), 8)
        self.assertEqual(sum(map(len, panel.PARKING_BRAKE.values())), 4)
        for mode in ("--brake-regen", "--parking-brake"):
            with patch.object(panel, "interface_info") as interface, patch.object(panel, "bound_socket") as network:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(panel.main([mode]), 0)
                interface.assert_not_called()
                network.assert_not_called()
        self.assertEqual(panel.request_frame(0x18, 0xDA31).hex(), "000000050001f41822da31")
        for raw, value in ((b"\x00\x00", 0), (b"\x00\x01", 1), (b"\xff\xff", None), (b"\x00\x02", None)):
            self.assertEqual(panel.decode(0x18, 0xDA31, raw)["fields"]["parking_brake_status"]["value"], value)
        for did in (0x46B4, 0x43BF, 0x586D):
            result = panel.decode(0x12, did, b"\x01")
            self.assertIsNone(result["active_recuperation"])
            self.assertFalse(result["enum_semantics_verified"])
        counters = bytes.fromhex("0000000affffffec0000001e01000000")
        fields = panel.decode(0x12, 0x40C0, counters)["fields"]
        self.assertEqual(fields["recuperation_charge_since_battery_change"]["value"], 1)
        self.assertEqual(fields["balancehold_charge_since_battery_change"]["value"], -2)
        self.assertEqual(fields["recuperation_charge_bucket"]["value"], 1)
        self.assertEqual(panel.decode(0x12, 0x40C0, counters[:-1])["status"], "layout_mismatch")
        with patch.object(
            panel, "read_one", return_value={"status": "positive", "length": 3, "data_hex": "0f1fe8"}
        ) as read:
            panel.collect(None, {0x12: panel.BRAKE_REGEN[0x12]}, {0x12: "0f1ff0"})
            self.assertEqual(read.call_count, 1)

    def test_missing_panel_plan_and_source_scales(self):
        self.assertEqual(sum(len(r) for r in panel.PANEL_MISSING.values()), 8)
        self.assertAlmostEqual(
            panel.decode(0x12, 0x43C2, bytes([187]))["fields"]["battery_soc_energy_management"]["value"], 60
        )
        self.assertAlmostEqual(
            panel.decode(0x12, 0x48A5, bytes.fromhex("6000"))["fields"]["actual_engine_torque"]["value"], 312.54281216
        )
        self.assertEqual(
            panel.decode(0x18, 0xDA27, bytes.fromhex("0700"))["fields"]["gear_lever_position"]["label"], "M/S"
        )
        self.assertFalse(panel.decode(0x18, 0xDA27, bytes.fromhex("0701"))["fields"]["gear_lever_position"]["valid"])
        for ecu, did in ((0x12, 0x48A5), (0x18, 0xDA27)):
            self.assertEqual(panel.decode(ecu, did, b"\x00")["status"], "layout_mismatch")
        with patch.object(panel, "interface_info") as interface, patch.object(panel, "bound_socket") as network:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(panel.main(["--panel-missing"]), 0)
            interface.assert_not_called()
            network.assert_not_called()

    def test_fixed_engine_load_obd_and_uds_frames(self):
        self.assertEqual(panel.request_frame(0x12, 0x0104).hex(), "000000040001f4120104")
        self.assertEqual(panel.request_frame(0x12, 0xF404).hex(), "000000050001f41222f404")
        result = panel.read_one(Wire(frame(0x12, bytes.fromhex("410433"))), 0x12, 0x0104)
        self.assertEqual(result["status"], "positive")
        self.assertEqual(result["interpretation"]["fields"]["calculated_engine_load_obd"]["value"], 20)
        result = panel.read_one(Wire(frame(0x12, bytes.fromhex("7f0131"))), 0x12, 0x010D)
        self.assertEqual(result["status"], "negative")

    def test_d83_source_formats_and_exact_identity_gate(self):
        self.assertEqual(sum(len(records) for records in panel.D83.values()), 8)
        self.assertNotIn(0x586F, panel.D83[0x12])
        self.assertNotIn(0x402B, panel.D83[0x12])
        self.assertEqual(panel.request_frame(0x12, 0x4286).hex(), "000000050001f412224286")
        fields = panel.decode(0x12, 0x4286, bytes.fromhex("08ca"))["fields"]
        self.assertAlmostEqual(fields["battery_current_ibs"]["value"], -20)
        temp = panel.decode(0x12, 0x428C, bytes.fromhex("1b11"))["fields"]["battery_temperature_ibs"]["value"]
        self.assertAlmostEqual(temp, 14.00351945)
        self.assertEqual(
            panel.decode(0x12, 0x42B9, bytes.fromhex("0bb8"))["fields"]["oil_pressure_corrected_relative"]["value"],
            3000,
        )
        self.assertEqual(panel.decode(0x12, 0x4286, b"\x00")["status"], "layout_mismatch")
        with patch.object(
            panel, "read_one", return_value={"status": "positive", "length": 3, "data_hex": "0f1fe8"}
        ) as read:
            results = panel.collect(None, panel.D83, {0x12: "0f1ff0"})
            self.assertEqual(read.call_count, 1)
            self.assertIn("candidates_skipped", results[0])
        with patch.object(panel, "interface_info") as interface, patch.object(panel, "bound_socket") as network:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(panel.main(["--d83"]), 0)
            interface.assert_not_called()
            network.assert_not_called()

    def test_d83_current_offsets_preserve_both_signs(self):
        self.assertEqual(sum(len(records) for records in panel.D83_CURRENT.values()), 6)
        for raw, expected in (("7fff", 0), ("7ff5", -0.5), ("8009", 0.5)):
            fields = panel.decode(0x12, 0x43C0, bytes.fromhex(raw))["fields"]
            self.assertAlmostEqual(fields["battery_current_energy_management"]["value"], expected)
        self.assertEqual(panel.decode(0x12, 0x42EE, b"\x00\x64")["fields"]["storage_current_available"]["value"], 5)
        with patch.object(panel, "interface_info") as interface, patch.object(panel, "bound_socket") as network:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(panel.main(["--d83-current"]), 0)
            interface.assert_not_called()
            network.assert_not_called()

    def test_d83_captured_responses_decode_with_profile_specific_scales(self):
        cases = [
            (0x42B9, "0507", "oil_pressure_corrected_relative", 1287),
            (0x43C0, "7fa3", "battery_current_energy_management", -4.6),
            (0x5868, "86", "battery_temperature_available", 8),
            (0x5866, "39e1", "battery_voltage_available", 14.817),
        ]
        for did, raw, name, expected in cases:
            with self.subTest(did=hex(did)):
                wire = Wire(frame(0x12, b"\x62" + did.to_bytes(2, "big") + bytes.fromhex(raw)))
                result = panel.read_one(wire, 0x12, did)
                self.assertEqual(result["status"], "positive")
                self.assertAlmostEqual(result["interpretation"]["fields"][name]["value"], expected)
                self.assertEqual(len(wire.sent), 1)

    def test_offline_default_never_opens_network(self):
        with patch.object(panel, "interface_info") as interface, patch.object(panel, "bound_socket") as network:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(panel.main([]), 0)
            interface.assert_not_called()
            network.assert_not_called()

    def test_allowlist_and_request_format(self):
        self.assertEqual(panel.request_frame(0x12, 0x402B).hex(), "000000050001f41222402b")
        for ecu, did in ((0x10, 0x402B), (0x18, 0x586F), (0x12, 0xF190), (0x18, 0xFFFF)):
            with self.assertRaises(ValueError):
                panel.request_frame(ecu, did)
        self.assertEqual(sum(len(records) for records in panel.RECORDS.values()), 8)

    def test_battery_sign_and_independent_nulls(self):
        fields = panel.decode(0x12, 0x402B, bytes.fromhex("ffc987f008ca"))["fields"]
        self.assertEqual(fields["battery_temperature"]["value"], -5.5)
        self.assertAlmostEqual(fields["voltage"]["value"], 14.7)
        self.assertEqual(fields["battery_current"]["value"], -20)
        fields = panel.decode(0x12, 0x402B, bytes.fromhex("ffc9ffff08ca"))["fields"]
        self.assertFalse(fields["voltage"]["valid"])
        self.assertTrue(fields["battery_current"]["valid"])
        self.assertTrue(all(not f["valid"] for f in panel.decode(0x12, 0x402B, b"\xff" * 6)["fields"].values()))

    def test_transmission_enums_signed_speed_and_layout(self):
        fields = panel.decode(0x18, 0xDA2E, b"\x10\x04")["fields"]
        self.assertEqual(fields["gear_selector"]["value"], "D")
        self.assertEqual(fields["gear_mode"]["value"], "M")
        self.assertFalse(panel.decode(0x18, 0xDA2E, b"\x10\x05")["fields"]["gear_mode"]["valid"])
        self.assertFalse(panel.decode(0x18, 0xDA22, b"\x00\xff")["fields"]["converter_lock"]["valid"])
        fields = panel.decode(0x18, 0xDA2A, bytes.fromhex("0320ff9c"))["fields"]
        self.assertEqual(fields["converter_rpm"]["value"], 800)
        self.assertEqual(fields["output_shaft_rpm"]["value"], -100)
        self.assertEqual(panel.decode(0x12, 0x586F, b"\x00\x00")["fields"]["oil_pressure"]["value"], 0)
        self.assertEqual(panel.decode(0x18, 0x586F, b"\x00\x00")["status"], "layout_mismatch")
        self.assertEqual(panel.decode(0x12, 0x402B, b"\x00" * 7)["status"], "layout_mismatch")

    def test_actual_profile_oil_reply_is_not_reinterpreted_as_pressure(self):
        # Captured 2026-11-17, ECU12 SGBD0F1FF0: 62 58 6F C8. The
        # two-byte catalog formula does not apply; do not invent a scale.
        result = panel.decode(0x12, 0x586F, bytes.fromhex("c8"))
        self.assertEqual(result["status"], "layout_mismatch")
        self.assertNotIn("fields", result)
        fields = panel.decode(0x18, 0xDA2A, bytes.fromhex("02f10000"))["fields"]
        self.assertEqual(fields["converter_rpm"]["value"], 753)
        self.assertEqual(fields["output_shaft_rpm"]["value"], 0)

    def test_alive_pending_wrong_target_and_complete_response(self):
        alive = struct.pack("!IH", 0, 0x12)
        wire = Wire(
            alive
            + frame(0x12, b"\x7f\x22\x78")
            + frame(0x18, bytes.fromhex("62586f0000"))
            + frame(0x12, bytes.fromhex("62586f0fa0"))
        )
        result = panel.read_one(wire, 0x12, 0x586F)
        self.assertEqual(result["status"], "positive")
        self.assertEqual(result["data_hex"], "0fa0")
        self.assertEqual(len(wire.sent), 2)
        self.assertEqual(wire.sent[1].hex(), "00000002001300f4")

    def test_transport_errors_stop_bounded_receiver(self):
        cases = [
            (frame(0x12, b"", 0x40), "transport_rejected"),
            (struct.pack("!IH", 513, 1), "incomplete"),
            (b"", "incomplete"),
            (frame(0x18, b"abc") * 32, "frame_limit"),
        ]
        for wire, expected in cases:
            with self.subTest(expected=expected):
                self.assertEqual(panel.read_one(Wire(wire), 0x12, 0x586F)["status"], expected)
        negative = panel.read_one(Wire(frame(0x12, b"\x7f\x22\x31")), 0x12, 0x586F)
        self.assertEqual(negative["status"], "negative")
        self.assertEqual(negative["nrc"], "0x31")

    def test_absolute_deadline_not_extended(self):
        with patch.object(panel.time, "monotonic", side_effect=[0, 4]):
            wire = Wire(frame(0x12, b"\x7f\x22\x78"))
            self.assertEqual(panel.read_one(wire, 0x12, 0x586F)["status"], "incomplete")
            self.assertEqual(len(wire.sent), 1)

    def test_collect_identity_gates_and_global_stop(self):
        with patch.object(
            panel,
            "read_one",
            side_effect=[{"status": "negative"}, {"status": "positive", "length": 3, "data_hex": "abcdef"}],
        ) as read:
            panel.collect(None)
            self.assertEqual(read.call_count, 2)

    def test_followup_is_separate_bounded_and_identity_gated(self):
        self.assertEqual(sum(len(records) for records in panel.FOLLOWUP.values()), 7)
        self.assertEqual(panel.request_frame(0x18, 0x600C).hex(), "000000050001f41822600c")
        self.assertEqual(panel.decode(0x12, 0xF197, b"DME-test\x00")["system_name_ascii"], "DME-test")
        self.assertEqual(panel.decode(0x18, 0x600A, b"\x18")["fields"]["native_engine_rpm"]["value"], 768)
        self.assertEqual(panel.decode(0x18, 0x600C, b"\x00")["fields"]["native_current_gear"]["value"], 0)
        with patch.object(
            panel,
            "read_one",
            side_effect=[{"status": "negative"}, {"status": "positive", "length": 3, "data_hex": "abcdef"}],
        ) as read:
            results = panel.collect(None, panel.FOLLOWUP)
            self.assertEqual(read.call_count, 2)
            self.assertTrue(all("candidates_skipped" in r for r in results))
        with patch.object(panel, "interface_info") as interface, patch.object(panel, "bound_socket") as network:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(panel.main(["--followup"]), 0)
            interface.assert_not_called()
            network.assert_not_called()
            self.assertTrue(all("candidates_skipped" in r for r in results))
        with patch.object(panel.time, "sleep"), patch.object(
            panel,
            "read_one",
            side_effect=[{"status": "positive", "length": 3, "data_hex": "123456"}, {"status": "incomplete"}],
        ) as read:
            self.assertEqual(len(panel.collect(None)), 2)
            self.assertEqual(read.call_count, 2)


if __name__ == "__main__":
    unittest.main()
