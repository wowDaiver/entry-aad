#!/usr/bin/env python3
"""Generate a preview from the real native decoder's synthetic replay outputs."""

import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
events = [
    (0, 1, {"temperature": {"engine": 80, "oil": 76}}),
    (1, 1, {"RPMSpeed": 678}),
    (2, 1, {"speedActual": 0}),
    (3, 1, {"averageConsumption": {"averageConsumption1": 8.2, "unit": 1}}),
    (4, 1, {"battery": 72}),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    cases = {
        "values": (events, 1),
        "waiting": ([], 0),
        "stale": (events, 31),
        "error": ([(i, 1, {"error": 401}) for i in range(5)], 1),
    }
    scenarios = {}
    for name, (records, now) in cases.items():
        payload = "".join(f'{index} {time} {json.dumps(body,separators=(",",":"))}\n' for index, time, body in records)
        output = subprocess.check_output(
            [str(ROOT / "build/telemetry-replay-host"), str(now)], input=payload, text=True
        )
        scenarios[name] = dict(source="synthetic-native-replay", readings=json.loads(output))
    (ROOT / "build/replay-scenarios.json").write_text(json.dumps(scenarios, indent=2) + "\n")
    fragment = (
        (ROOT / "preview.fragment.html")
        .read_text()
        .replace("__NATIVE_SCENARIOS__", json.dumps(scenarios, separators=(",", ":")))
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(fragment)
    print(
        json.dumps(
            dict(
                output=str(args.out),
                scenarios=list(scenarios),
                source="native C++ decoder, synthetic inputs",
                vehicle_reads=0,
            )
        )
    )


if __name__ == "__main__":
    main()
