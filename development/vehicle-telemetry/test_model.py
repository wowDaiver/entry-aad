import json
import unittest

from model import Model


class ModelTests(unittest.TestCase):
    def test_all_six_candidates_without_double_scaling(self):
        m = Model(0)
        m.update("engine.temperature", '{"temperature":{"engine":80,"oil":76}}', 1)
        m.update("engine.RPMSpeed", '{"RPMSpeed":678}', 1)
        m.update("driving.speedActual", '{"speedActual":0}', 1)
        m.update(
            "driving.averageConsumption",
            '{"averageConsumption":{"averageConsumption1":8.2,"averageConsumption2":9.1,"unit":1}}',
            1,
        )
        m.update("sensors.battery", '{"battery":72}', 1)
        s = m.snapshot(1)
        self.assertEqual(len(s), 31)
        self.assertEqual(
            [
                s[k]["value"]
                for k in ("engine_temperature", "oil_temperature", "rpm", "speed", "average_consumption", "battery_soc")
            ],
            [80, 76, 678, 0, 8.2, 72],
        )
        self.assertEqual(sum(v["state"] == "valid" for v in s.values()), 6)
        self.assertIsNone(s["battery_soc"]["measured_at"])
        self.assertFalse(s["battery_soc"]["meaning_verified_on_146"])

    def test_zero_and_full_battery_are_values(self):
        for v in (0, 100):
            m = Model(0)
            m.update("sensors.battery", json.dumps({"battery": v}), 1)
            self.assertEqual(m.snapshot(1)["battery_soc"]["value"], v)

    def test_invalid_json_types_duplicates_and_sentinels(self):
        bad = [
            "{}",
            "null",
            "[]",
            '{"battery":true}',
            '{"battery":"72"}',
            '{"battery":null}',
            '{"battery":255}',
            '{"battery":-1}',
            '{"battery":72.1}',
            '{"battery":NaN}',
            '{"battery":Infinity}',
            '{"battery":1e999}',
            '{"battery":72,"battery":73}',
            '{"error":401,"battery":72}',
            '{"x":{"battery":72}}',
            '{"battery":72} trailing',
            '{"battery":' + "9" * 900 + "}",
            "x" * 1025,
        ]
        for raw in bad:
            with self.subTest(raw=raw[:70]):
                m = Model(0)
                m.update("sensors.battery", raw, 1)
                self.assertEqual(m.snapshot(1)["battery_soc"]["state"], "unavailable")
                self.assertIsNone(m.snapshot(1)["battery_soc"]["value"])

    def test_partial_temperature_reply_does_not_erase_sibling(self):
        m = Model(0)
        m.update("engine.temperature", '{"temperature":{"engine":80}}', 1)
        s = m.snapshot(1)
        self.assertEqual(s["engine_temperature"]["state"], "valid")
        self.assertEqual(s["oil_temperature"]["state"], "unavailable")

    def test_errors_do_not_refresh_last_good_timestamp(self):
        m = Model(0)
        m.update("sensors.battery", '{"battery":72}', 1)
        m.update("sensors.battery", '{"error":401}', 20)
        s = m.snapshot(20)["battery_soc"]
        self.assertEqual((s["state"], s["value"], s["received_at"]), ("stale", 72, 1))
        m.update("sensors.battery", '{"battery":73}', 21)
        self.assertEqual(m.snapshot(21)["battery_soc"]["state"], "valid")

    def test_loading_and_independent_freshness(self):
        m = Model(0)
        self.assertEqual(m.snapshot(0)["rpm"]["state"], "loading")
        self.assertEqual(m.snapshot(5)["rpm"]["state"], "unavailable")
        m.update("engine.RPMSpeed", '{"RPMSpeed":0}', 1)
        m.update("sensors.battery", '{"battery":72}', 1)
        s = m.snapshot(3)
        self.assertEqual(s["rpm"]["state"], "stale")
        self.assertEqual(s["battery_soc"]["state"], "valid")
        self.assertEqual(m.snapshot(31)["battery_soc"]["state"], "stale")

    def test_source_units_are_retained(self):
        for code, unit in ((1, "l/100km"), (2, "mpg_uk"), (3, "mpg_us"), (4, "km/l")):
            m = Model(0)
            m.update(
                "driving.averageConsumption",
                json.dumps({"averageConsumption": {"averageConsumption1": 27.2, "unit": code}}),
                1,
            )
            self.assertEqual(m.snapshot(1)["average_consumption"]["unit"], unit)
        for code in (0, 5, True):
            m = Model(0)
            m.update(
                "driving.averageConsumption",
                json.dumps({"averageConsumption": {"averageConsumption1": 27.2, "unit": code}}),
                1,
            )
            self.assertEqual(m.snapshot(1)["average_consumption"]["state"], "unavailable")

    def test_sentinel_replies(self):
        for prop, raw, key in [
            ("engine.RPMSpeed", '{"RPMSpeed":65535}', "rpm"),
            ("driving.speedActual", '{"speedActual":65535}', "speed"),
            ("engine.temperature", '{"temperature":{"engine":207,"oil":207}}', "oil_temperature"),
            (
                "driving.averageConsumption",
                '{"averageConsumption":{"averageConsumption1":4093,"unit":1}}',
                "average_consumption",
            ),
        ]:
            m = Model(0)
            m.update(prop, raw, 1)
            self.assertEqual(m.snapshot(1)[key]["state"], "unavailable")

    def test_clock_and_unknown_property(self):
        for v in (-1, float("nan"), True):
            with self.assertRaises(ValueError):
                Model(v)
        m = Model(2)
        for v in (1, float("inf"), True):
            with self.assertRaises(ValueError):
                m.update("sensors.battery", "{}", v)
        with self.assertRaises(ValueError):
            m.update("sensors.SOCBatteryHybrid", "{}", 3)
        with self.assertRaises(ValueError):
            m.snapshot(1)


if __name__ == "__main__":
    unittest.main()
