"""Compare actual native decoder execution with the existing host reference."""

import json
import subprocess
import unittest
from pathlib import Path

from model import METRICS, Model

ROOT = Path(__file__).resolve().parent
PROPS = [
    "engine.temperature",
    "engine.RPMSpeed",
    "driving.speedActual",
    "driving.averageConsumption",
    "sensors.battery",
]


class NativeReplayTests(unittest.TestCase):
    def compare(self, events, now):
        data = "".join(f"{prop} {time} {raw}\n" for prop, time, raw in events)
        result = subprocess.run(
            [str(ROOT / "build/telemetry-replay-host"), str(now)],
            input=data,
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        native = json.loads(result.stdout)
        ref = Model(0)
        for prop, time, raw in events:
            ref.update(PROPS[prop], raw, time)
        reference = ref.snapshot(now)
        for key in native:
            self.assertEqual(native[key]["state"], reference[key]["state"], key)
            self.assertEqual(native[key]["value"], reference[key]["value"], key)
            if key in METRICS:
                self.assertEqual(native[key]["received_at"], reference[key]["received_at"])
            self.assertIsNone(native[key]["measured_at"])
            self.assertFalse(native[key]["meaning_verified_on_146"])
        return native

    def test_all_metrics_and_no_rescale(self):
        self.compare(
            [
                (0, 1, '{"temperature":{"engine":80,"oil":76}}'),
                (1, 1, '{"RPMSpeed":678}'),
                (2, 1, '{"speedActual":0}'),
                (3, 1, '{"averageConsumption":{"averageConsumption1":8.2,"unit":1}}'),
                (4, 1, '{"battery":72}'),
            ],
            1,
        )

    def test_errors_and_sentinels(self):
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
            '{"battery":01}',
            '{"battery":0x48}',
            '{"battery":+72}',
            '{"battery":72,}',
            '{"battery":72,"other":[1,]}',
            '{"battery":72,"meta":"bad\\q"}',
        ]
        for raw in bad:
            with self.subTest(raw=raw[:60]):
                self.compare([(4, 1, raw)], 1)

    def test_empty_zero_full_partial_and_freshness(self):
        self.compare([], 0)
        self.compare([], 5)
        for value in (0, 100):
            self.compare([(4, 1, json.dumps({"battery": value}))], 1)
        self.compare([(0, 1, '{"temperature":{"engine":80}}')], 1)
        self.compare([(1, 1, '{"RPMSpeed":0}'), (4, 1, '{"battery":72}')], 3)
        self.compare([(4, 1, '{"battery":72}')], 31)

    def test_error_keeps_old_value_and_can_recover(self):
        events = [(4, 1, '{"battery":72}'), (4, 20, '{"error":401}')]
        self.compare(events, 20)
        self.compare(events + [(4, 21, '{"battery":73}')], 21)

    def test_units_and_unknown_structures(self):
        for code in (1, 2, 3, 4, 0, 5, True, 1.0):
            raw = json.dumps({"averageConsumption": {"averageConsumption1": 27.2, "unit": code}})
            self.compare([(3, 1, raw)], 1)
        self.compare([(4, 1, '{"battery":72,"meta":[null,true,{"text":"ok\\n\\u0041"}]}')], 1)
        self.compare([(0, 1, '{"temperature":{"engine":80,"oil":76,"oil":77}}')], 1)

    def test_native_limits_are_fail_closed(self):
        for raw in [
            '{"battery":72,"a":' + "[" * 10 + "0" + "]" * 10 + "}",
            '{"battery":72,"a":[' + ",".join("0" for _ in range(100)) + "]}",
            '{"batt\\u0065ry":72}',
        ]:
            # Valid general JSON, deliberately outside the bounded native format.
            data = f"4 1 {raw}\n"
            native = json.loads(
                subprocess.check_output([str(ROOT / "build/telemetry-replay-host"), "1"], input=data, text=True)
            )
            self.assertEqual(native["battery_soc"]["state"], "unavailable")


if __name__ == "__main__":
    unittest.main()
