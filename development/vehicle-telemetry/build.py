#!/usr/bin/env python3
"""Host regressions, ARM object build and 146 import verification; no car access."""

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parents[1]
sys.path.insert(0, str(PROJECT / "development/abi-146/scripts"))
from elf32 import ELF32  # noqa: E402


def main():
    out = ROOT / "build"
    out.mkdir(exist_ok=True)
    commands = []

    def run(cmd, cwd=ROOT):
        commands.append(list(map(str, cmd)))
        result = subprocess.run(
            list(map(str, cmd)), cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )
        if result.returncode:
            (out / "failure.log").write_text(result.stdout)
            print(result.stdout, file=sys.stderr)
            raise RuntimeError("command failed; see build/failure.log")
        return result.stdout

    logs = run([sys.executable, PROJECT / "research/evidence/vehicle-menu-146/inspect.py"])
    logs += run(
        [
            sys.executable,
            "-m",
            "unittest",
            "-v",
            "test_model.py",
            "test_collect146.py",
            "test_resources.py",
            "test_source_mapping.py",
        ]
    )
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-g",
            "probe.cpp",
            "test_probe.cpp",
            "-o",
            out / "test-probe",
        ]
    )
    logs += run([out / "test-probe"])
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-g",
            "probe.cpp",
            "telemetry_model.cpp",
            "test_continuous.cpp",
            "-o",
            out / "test-continuous",
        ]
    )
    logs += run([out / "test-continuous"])
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DTELEMETRY_HOST_ABI_STUB",
            "-fsanitize=address,undefined",
            "-g",
            "native_cds.cpp",
            "probe.cpp",
            "test_native.cpp",
            "-o",
            out / "test-native",
        ]
    )
    logs += run([out / "test-native"])
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-g",
            "telemetry_model.cpp",
            "replay.cpp",
            "-o",
            out / "telemetry-replay-host",
        ]
    )
    logs += run([sys.executable, "-m", "unittest", "-v", "test_replay.py"])
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DTELEMETRY_HOST_ABI_STUB",
            "-fsanitize=address,undefined",
            "-g",
            "native_cds.cpp",
            "probe.cpp",
            "telemetry_model.cpp",
            "egs_reader.cpp",
            "controller.cpp",
            "test_controller.cpp",
            "-o",
            out / "test-controller",
        ]
    )
    logs += run([out / "test-controller"])
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-g",
            "test_menu_visibility.cpp",
            "-o",
            out / "test-menu-visibility",
        ]
    )
    logs += run([out / "test-menu-visibility"])
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-g",
            "test_trial_guard.cpp",
            "-o",
            out / "test-trial-guard",
        ]
    )
    logs += run([out / "test-trial-guard"])
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-g",
            "test_persistent_guard.cpp",
            "-o",
            out / "test-persistent-guard",
        ]
    )
    logs += run([out / "test-persistent-guard"])
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DTELEMETRY_HOST_ABI_STUB",
            "-fsanitize=address,undefined",
            "-g",
            "egs_reader.cpp",
            "telemetry_model.cpp",
            "test_egs.cpp",
            "-o",
            out / "test-egs",
        ]
    )
    logs += run([out / "test-egs"])
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-g",
            "telemetry_model.cpp",
            "test_diagnostics.cpp",
            "-o",
            out / "test-diagnostics",
        ]
    )
    logs += run([out / "test-diagnostics"])
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-misleading-indentation",
            "-fsanitize=address,undefined",
            "-g",
            "telemetry_model.cpp",
            "test_recording.cpp",
            "-o",
            out / "test-recording",
        ]
    )
    logs += run([out / "test-recording"])
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-misleading-indentation",
            "-fsanitize=address,undefined",
            "-g",
            "telemetry_model.cpp",
            "test_instrument_sources.cpp",
            "-o",
            out / "test-instrument-sources",
        ]
    )
    logs += run([out / "test-instrument-sources"])
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DTELEMETRY_HOST_ABI_STUB",
            "-fsanitize=address,undefined",
            "-g",
            "egs_reader.cpp",
            "test_diagnostic_transport.cpp",
            "-o",
            out / "test-diagnostic-transport",
        ]
    )
    logs += run([out / "test-diagnostic-transport"])
    logs += run([sys.executable, "-m", "unittest", "-v", "test_panel_sources.py"], cwd=PROJECT / "diagnostics")
    compiler = PROJECT.parent / "entrynav2-qemu/lab/qemu-runtime/software/bin/cxx"
    objects = []
    for name in ("probe", "native_cds", "telemetry_model", "egs_reader", "controller"):
        obj = out / (name + ".o")
        objects.append(obj)
        logs += run(
            [
                compiler,
                "-O2",
                "-fPIC",
                "-fvisibility=hidden",
                "-fvisibility-inlines-hidden",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-command-line-argument",
                "-c",
                ROOT / (name + ".cpp"),
                "-o",
                obj,
            ]
        )
    run(["/opt/homebrew/opt/llvm/bin/llvm-ar", "rcs", out / "libVehicleTelemetry.a", *objects])
    logs += run(
        [
            compiler,
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            ROOT / "replay.cpp",
            out / "telemetry_model.o",
            "-o",
            out / "telemetry-replay.elf",
        ]
    )
    # Object files are inspected, not executed or installed. No need to rewrite
    # firmware DSOs or claim that transitive runtime dependencies are resolved.
    imports = sorted(
        {
            s["name"]
            for obj in objects
            for t in ELF32(obj).symbol_tables.values()
            for s in t
            if not s["section"] and "3bmw" in s["name"]
        }
    )
    originals = PROJECT / "research/evidence/hu-compatibility-2026-11-10/files/opt/hmi/lib"
    providers = {name: ELF32(originals / name) for name in ("libAppFramework.A.so", "libCoreFramework.A.so")}
    # Generate the candidate guard only for the exact archived physical HU.
    hmi = ELF32(originals.parent / "bin/HmiMain3D")
    pinned = {
        "Hmi146": "b5549e3078e3a347b8f1bf856a36e13c456547db",
        "App146": "8b8ced1e9ea0dcd128ce796a500002b9dd0ed83a",
        "Core146": "d10bcc913025384622600cffd9686f2b63979102",
    }
    found = [
        hmi.metadata()["build_id"],
        providers["libAppFramework.A.so"].metadata()["build_id"],
        providers["libCoreFramework.A.so"].metadata()["build_id"],
    ]
    if found != list(pinned.values()):
        raise ValueError("foreign firmware inputs")
    table = json.loads((PROJECT / "development/abi-146/evidence/_ZTVN3bmw9PluginAppE.vtable.json").read_text())
    slots = sorted((r for r in table["rows"] if "primary_slot" in r), key=lambda r: r["primary_slot"])
    if [r["primary_slot"] for r in slots] != list(range(34)):
        raise ValueError("incomplete vtable evidence")
    header = "".join(
        "static const unsigned char "
        + name
        + "[]={"
        + ",".join("0x" + value[i : i + 2] for i in range(0, 40, 2))
        + "};\n"
        for name, value in pinned.items()
    )
    header += "static const char* ExpectedSlots[34]={" + ",".join(json.dumps(r["symbol"]) for r in slots) + "};\n"
    (out / "profile146.hpp").write_text(header)
    logs += run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DTELEMETRY_HOST_ABI_STUB",
            "-fsanitize=address,undefined",
            "-g",
            "-I" + str(out),
            "-I" + str(ROOT / "tests_support"),
            "plugin.cpp",
            "native_cds.cpp",
            "probe.cpp",
            "telemetry_model.cpp",
            "egs_reader.cpp",
            "controller.cpp",
            "test_factory.cpp",
            "-o",
            out / "test-factory",
        ]
    )
    logs += run(
        [
            out / "test-factory",
            originals / "libAppFramework.A.so",
            PROJECT / "development/abi-146/inputs/156-psdz/opt/hmi/lib/libAppFramework.A.so",
            pinned["App146"],
        ]
    )
    logs += run(
        [
            compiler,
            "-O2",
            "-fPIC",
            "-fvisibility=hidden",
            "-fvisibility-inlines-hidden",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-command-line-argument",
            "-I" + str(out),
            "-c",
            ROOT / "plugin.cpp",
            "-o",
            out / "plugin.o",
        ]
    )
    plugin_imports = {
        s["name"]
        for t in ELF32(out / "plugin.o").symbol_tables.values()
        for s in t
        if not s["section"] and "3bmw" in s["name"]
    }
    imports = sorted(set(imports) | plugin_imports)
    # Isolated LINK-ONLY normalization of malformed LOCAL markers for LLD.
    # Original/runtime DSOs remain byte-for-byte unchanged.
    linkdir = out / "link-only"
    linkdir.mkdir(exist_ok=True)
    normalized = []
    cpp_original = originals.parents[2] / "lib/libstdc++.so.6.0.17"
    link_modules = dict(providers)
    link_modules[cpp_original.name] = ELF32(cpp_original)
    for name, elf in link_modules.items():
        source = cpp_original if name == cpp_original.name else originals / name
        data = bytearray(source.read_bytes())
        patches = []
        for index, syms in elf.symbol_tables.items():
            section = elf.sections[index]
            if section["type"] != 11:
                continue
            for number, symbol in enumerate(syms):
                if number < section["info"] or symbol["binding"] != 0:
                    continue
                if symbol["name"] not in {
                    "_edata",
                    "__end__",
                    "__bss_end__",
                    "_bss_end__",
                    "__bss_start__",
                    "_end",
                    "__bss_start",
                }:
                    raise ValueError("unexpected malformed ELF symbol")
                offset = section["offset"] + number * 16 + 12
                data[offset] = (data[offset] & 15) | 16
                patches.append(symbol["name"])
        (linkdir / name).write_bytes(data)
        normalized.append(
            dict(name=name, link_only=True, sha256=hashlib.sha256(data).hexdigest(), changed_bindings=patches)
        )
    candidate = out / "libVehicleTelemetry.146-candidate.so"
    (out / "exports.map").write_text("{ global: pluginAppInfo; local: *; };\n")
    logs += run(
        [
            compiler,
            "-shared",
            "-nostdlib++",
            "-Wl,-soname,libVehicleTelemetry.so",
            *objects,
            out / "plugin.o",
            "-L" + str(linkdir),
            "-Wl,--allow-shlib-undefined",
            "-l:libAppFramework.A.so",
            "-l:libCoreFramework.A.so",
            "-l:libstdc++.so.6.0.17",
            "-Wl,--version-script=" + str(out / "exports.map"),
            "-ldl",
            "-lrt",
            "-o",
            candidate,
        ]
    )
    candidate_elf = ELF32(candidate)
    exports = sorted(
        {
            s["name"]
            for i, t in candidate_elf.symbol_tables.items()
            if candidate_elf.sections[i]["type"] == 11
            for s in t
            if s["section"] and s["binding"] in (1, 2) and s["other"] & 3 == 0
        }
    )
    if exports != ["pluginAppInfo"]:
        raise ValueError("unexpected plugin exports: " + str(exports))
    if "libstdc++.so.6" not in candidate_elf.metadata()["needed"]:
        raise ValueError("plugin must use the native shared C++ runtime")
    imports = sorted(
        {s["name"] for t in candidate_elf.symbol_tables.values() for s in t if not s["section"] and "3bmw" in s["name"]}
    )
    resolved = {name: [lib for lib, elf in providers.items() if name in elf.symbols] for name in imports}
    if not imports or any(not libs for libs in resolved.values()):
        raise ValueError("missing BMW imports: " + str(resolved))
    from build_resources import build as build_resources

    menu_bundle = build_resources(candidate, out / "native-menu")
    (out / "checks.log").write_text(logs)
    report = dict(
        scope="host tests + ARM archive/import inspection only",
        installed=False,
        live_vehicle_reads=0,
        bmw_imports=resolved,
        native_modules={
            lib: dict(
                sha256=hashlib.sha256((originals / lib).read_bytes()).hexdigest(), build_id=elf.metadata()["build_id"]
            )
            for lib, elf in providers.items()
        },
        commands=commands,
        sources={
            p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in ROOT.iterdir()
            if p.suffix in (".cpp", ".hpp", ".py", ".xml", ".manifest")
        },
        output=dict(
            path=str(out / "libVehicleTelemetry.a"),
            sha256=hashlib.sha256((out / "libVehicleTelemetry.a").read_bytes()).hexdigest(),
        ),
        arm_objects={p.name: ELF32(p).metadata() for p in objects},
        plugin_candidate=dict(
            path=str(candidate),
            sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
            exports=exports,
            elf=candidate_elf.metadata(),
            exported_factory="pluginAppInfo",
            profile_build_ids=pinned,
            native_execution_tested=False,
        ),
        link_only_inputs=normalized,
        native_menu=menu_bundle,
        python_tests=sum(int(n) for n in re.findall(r"Ran (\d+) tests? in", logs)),
        activation_policy="persistent-startup-guard",
        egs_network_policy="vehicle-egs-hsfz-v1",
        diagnostic_profiles={
            "0x12": {
                "sgbd": "0F1FF0",
                "dids": ["42B9", "43C0", "5868", "5866", "43C2", "48A5", "40C0"],
                "obd_pids": ["04"],
            },
            "0x18": {
                "sgbd": "0F21D0",
                "dids": ["6000", "DA2E", "DA22", "DA2A", "6009", "600A", "600C", "DA27", "DA31"],
            },
        },
        sanitized_cpp_suites=[
            "probe",
            "continuous_pipeline",
            "native_boundary",
            "controller",
            "menu_visibility",
            "trial_guard",
            "persistent_guard",
            "factory",
            "egs",
            "diagnostics",
            "diagnostic_transport",
            "recording",
            "instrument_sources",
        ],
        limitations=[
            "not a standalone executable or installable plugin",
            "native ABI, dispatcher scheduling and callback teardown require integration/live checks",
            (
                "native menu/resource bundle built; Custom placement in Vehicle Status "
                "is statically grounded but not live-tested"
            ),
            "native iDrive/Back rendering and registration remain unverified on the physical HU",
        ],
    )
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(logs)
    print(json.dumps(dict(arm_archive=report["output"], bmw_imports=len(imports), installed=False)))


if __name__ == "__main__":
    main()
