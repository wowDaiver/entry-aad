#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run host regressions without firmware, a VM, network or a connected vehicle."""

import argparse
import subprocess
import sys
from pathlib import Path

from host_tools import compiler, default_font, freetype_flags

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, help="Local non-distributed font for host rendering")
    args = parser.parse_args()
    font = (args.font or default_font()).resolve()
    out = ROOT / "build"
    out.mkdir(exist_ok=True)
    logs = []

    def run(command, cwd=ROOT):
        result = subprocess.run(list(map(str, command)), cwd=cwd, capture_output=True, text=True)
        logs.append(result.stdout + result.stderr)
        (out / "check-host.log").write_text("\n".join(logs))
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    cxx = compiler()
    flags = [
        "-std=c++11",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-misleading-indentation",
        "-fsanitize=address,undefined",
    ]
    suites = {
        "probe": ["probe.cpp", "test_probe.cpp"],
        "continuous": ["probe.cpp", "telemetry_model.cpp", "test_continuous.cpp"],
        "native": ["native_cds.cpp", "probe.cpp", "test_native.cpp"],
        "controller": [
            "native_cds.cpp",
            "probe.cpp",
            "telemetry_model.cpp",
            "egs_reader.cpp",
            "controller.cpp",
            "test_controller.cpp",
        ],
        "visibility": ["test_menu_visibility.cpp"],
        "trial": ["test_trial_guard.cpp"],
        "persistent": ["test_persistent_guard.cpp"],
        "egs": ["egs_reader.cpp", "telemetry_model.cpp", "test_egs.cpp"],
        "diagnostics": ["telemetry_model.cpp", "test_diagnostics.cpp"],
        "recording": ["telemetry_model.cpp", "test_recording.cpp"],
        "instruments": ["telemetry_model.cpp", "test_instrument_sources.cpp"],
        "transport": ["egs_reader.cpp", "test_diagnostic_transport.cpp"],
    }
    for name, sources in suites.items():
        exe = out / ("host-" + name)
        stub = ["-DTELEMETRY_HOST_ABI_STUB"] if name in ("native", "controller", "egs", "transport") else []
        run([cxx, *flags, *stub, *sources, "-o", exe])
        run([exe])
    run([cxx, *flags, "-DTELEMETRY_MOST_WORKER_TEST", "most_worker.cpp", "-o", out / "host-most-worker", "-ldl"])
    run([cxx, *flags, "-shared", "-fPIC", "tests_support/fake_vmost.cpp", "-o", out / "host-fake-vmost.so"])
    run([sys.executable, "-m", "unittest", "-v", "test_most_worker.py"])
    run([sys.executable, "-m", "unittest", "discover", "-s", "offline-tests", "-p", "test_most_install.py", "-v"])
    run([cxx, *flags, "telemetry_model.cpp", "replay.cpp", "-o", out / "telemetry-replay-host"])
    # Tests import the same source mapper, with an explicit host-owned font.
    script = "import unittest,test_source_mapping; test_source_mapping.mapping.FONT=" + repr(str(font))
    script += (
        ";suite=unittest.defaultTestLoader.loadTestsFromNames("
        + repr(["test_model", "test_collect146", "test_resources", "test_replay", "test_source_mapping"])
        + ");result=unittest.TextTestRunner(verbosity=2).run(suite);raise SystemExit(not result.wasSuccessful())"
    )
    run([sys.executable, "-c", script])
    run([sys.executable, "-m", "unittest", "-v", "test_panel_sources.py"], cwd=ROOT.parents[1] / "diagnostics")
    exe = out / "host-physical-graphics"
    run(
        [
            cxx,
            *flags,
            "-DTELEMETRY_HOST_ABI_STUB",
            "-DTELEMETRY_GRAPHICS",
            '-DTELEMETRY_DASHBOARD_FONT="' + str(font) + '"',
            *suites["controller"],
            "dashboard/renderer.cpp",
            *freetype_flags(),
            "-o",
            exe,
        ]
    )
    run([exe])
    run([sys.executable, "dashboard/check.py", "--font", font])
    print(
        "PASS: 64 Python tests, 12 core sanitizer suites, isolated MOST worker, "
        "physical graphics controller and PNG checks"
    )
    print("Factory/ARM firmware checks and complete private-kit checks require separate owner inputs.")


if __name__ == "__main__":
    main()
