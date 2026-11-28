#!/usr/bin/env python3
"""Build exact146 physical graphics with live Model/CDS; no emulator or vehicle I/O."""

import hashlib
import json
import os
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = Path(os.environ.get("VEHICLE_TELEMETRY_OWNER_ROOT", str(ROOT.parents[1]))).resolve()
QEMU = PROJECT.parent / "entrynav2-qemu"
BASE = ROOT / "build"
OUT = BASE / "graphics146"
sys.path.insert(0, str(PROJECT / "development/abi-146/scripts"))
sys.path.insert(0, str(ROOT))
from build_resources import build as resources  # noqa: E402
from build_resources import put  # noqa: E402
from elf32 import ELF32  # noqa: E402


def main():
    OUT.mkdir(exist_ok=True)
    logs = []

    def run(argv):
        result = subprocess.run(
            list(map(str, argv)), cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )
        logs.append(result.stdout)
        if result.returncode:
            (OUT / "failure.log").write_text(result.stdout)
            print(result.stdout, file=sys.stderr)
            result.check_returncode()
        return result.stdout

    compiler = QEMU / "lab/qemu-runtime/software/bin/cxx"
    font = QEMU / "lab/owner146-capture/files/opt/hmi/data/ro/mini/font/MINI_TA_RG.ttf"
    # Exercise the physical graphics controller under ASan/UBSan with a host font path.
    run(
        [
            "/opt/homebrew/opt/llvm/bin/clang++",
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-misleading-indentation",
            "-fsanitize=address,undefined",
            "-g",
            "-DTELEMETRY_HOST_ABI_STUB",
            "-DTELEMETRY_GRAPHICS",
            '-DTELEMETRY_DASHBOARD_FONT="' + str(font) + '"',
            "-I/opt/X11/include/freetype2",
            "native_cds.cpp",
            "probe.cpp",
            "telemetry_model.cpp",
            "egs_reader.cpp",
            "controller.cpp",
            "dashboard/renderer.cpp",
            "test_controller.cpp",
            "-L/opt/X11/lib",
            "-lfreetype",
            "-lz",
            "-o",
            OUT / "test-physical-graphics",
        ]
    )
    run([OUT / "test-physical-graphics"])
    baseline = json.loads((BASE / "report.json").read_text())
    for name in ("probe.cpp", "native_cds.cpp", "telemetry_model.cpp", "egs_reader.cpp"):
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != baseline["sources"][name]:
            raise ValueError("baseline object is stale: " + name)
    objects = [BASE / (n + ".o") for n in ("probe", "native_cds", "telemetry_model", "egs_reader")]
    for name, source in (
        ("controller", ROOT / "controller.cpp"),
        ("plugin", ROOT / "plugin.cpp"),
        ("renderer", ROOT / "dashboard/renderer.cpp"),
    ):
        obj = OUT / (name + ".o")
        objects.append(obj)
        run(
            [
                compiler,
                "-O2",
                "-fPIC",
                "-fvisibility=hidden",
                "-fvisibility-inlines-hidden",
                "-DTELEMETRY_GRAPHICS",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-misleading-indentation",
                "-Wno-unused-command-line-argument",
                "-I" + str(BASE),
                "-I/opt/X11/include/freetype2",
                "-c",
                source,
                "-o",
                obj,
            ]
        )
    candidate = OUT / "libVehicleTelemetry.graphics146.so"
    run(
        [
            compiler,
            "-shared",
            "-nostdlib++",
            "-Wl,-soname,libVehicleTelemetry.so",
            *objects,
            "-L" + str(BASE / "link-only"),
            "-Wl,--allow-shlib-undefined",
            "-l:libAppFramework.A.so",
            "-l:libCoreFramework.A.so",
            "-l:libstdc++.so.6.0.17",
            "-Wl,--version-script=" + str(BASE / "exports.map"),
            "-ldl",
            "-lrt",
            "-L" + str(QEMU / "lab/owner146-capture/files/opt/hmi/lib"),
            "-l:libfreetype2.so",
            "-L" + str(QEMU / "lab/vendor/qemu-lab/system/artifacts/native-wayland-build/sysroot/lib"),
            "-l:libz.so.1.2.8",
            "-o",
            candidate,
        ]
    )
    body = candidate.read_bytes()
    for forbidden in (
        b"VEHICLE_TELEMETRY_DEMO",
        b"isolated-owner146",
        b"telemetry-dashboard-scenario",
        b"telemetry-dashboard-recording",
    ):
        if forbidden in body:
            raise ValueError("emulator code leaked into physical candidate")
    elf = ELF32(candidate)
    providers = {
        n: ELF32(QEMU / "lab/owner146-capture/files/opt/hmi/lib" / n)
        for n in ("libAppFramework.A.so", "libCoreFramework.A.so")
    }
    imports = sorted(
        {
            s["name"]
            for index, table in elf.symbol_tables.items()
            if elf.sections[index]["type"] == 11
            for s in table
            if not s["section"] and "3bmw" in s["name"]
        }
    )
    if any(not any(name in provider.symbols for provider in providers.values()) for name in imports):
        raise ValueError("unresolved BMW image/controller imports")
    folder = OUT / "native-menu"
    resources(candidate, folder)
    menu = ET.parse(folder / "VehicleTelemetry.xml")
    plugin = menu.getroot().find("pluginApp")
    ET.SubElement(plugin.find("actions"), "hmiAction", id="44", target="15")
    ET.SubElement(plugin.find("models"), "raImageModel", id="120")
    ET.SubElement(plugin.find("models"), "textIdModel", id="24", textId="6")
    plugin.find("hmiStates/hmiState/components").insert(
        0, ET.Element("button", id="205", action="44", model="24", text="Приборы")
    )
    page = ET.SubElement(plugin.find("hmiStates"), "hmiState", id="15", parentId="10", text="Панель приборов")
    props = ET.SubElement(
        ET.SubElement(ET.SubElement(page, "components"), "image", id="290", model="120"), "properties"
    )
    for key, val in [(2, "false"), (3, "true"), (9, "1350"), (10, "477"), (20, "-90"), (21, "0")]:
        ET.SubElement(props, "property", id=str(key), value=val)
    ids = [n.get("id") for n in menu.getroot().iter() if n.get("id") is not None and n.tag != "property"]
    if len(ids) != len(set(ids)):
        raise ValueError("duplicate resource ID")
    menu.write(folder / "VehicleTelemetry.xml", encoding="utf-8", xml_declaration=True)
    archive = folder / "VehicleTelemetry_Texts.zip"
    with zipfile.ZipFile(archive) as src:
        texts = {n: src.read(n) for n in src.namelist()}
    with zipfile.ZipFile(archive, "w") as target:
        for name, text in texts.items():
            put(target, name, text + ("6=" + ("Приборы" if name == "ru.txt" else "Instruments") + "\n").encode())
    profile = json.loads((folder / "profile.json").read_text())
    profile.update(graphic_dashboard=True, synthetic_telemetry=False, mode="exact146 physical graphics candidate")
    (folder / "profile.json").write_text(json.dumps(profile, indent=2) + "\n")
    (folder / "SHA256SUMS").write_text(
        "".join(
            hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.name + "\n"
            for p in sorted(folder.iterdir())
            if p.is_file() and p.name != "SHA256SUMS"
        )
    )
    with zipfile.ZipFile(OUT / "VehicleTelemetry-native-menu-146-candidate.zip", "w") as target:
        for p in sorted(folder.iterdir()):
            if p.is_file():
                put(target, "VehicleTelemetry/" + p.name, p.read_bytes())
    (OUT / "checks.log").write_text("".join(logs))
    report = dict(
        kind="physical-graphics146",
        installed=False,
        synthetic_telemetry=False,
        persistent_startup_guard=True,
        candidate_sha256=hashlib.sha256(body).hexdigest(),
        candidate=str(candidate),
        resources=str(folder),
        elf=elf.metadata(),
        bmw_imports=imports,
        font_sha256=hashlib.sha256(font.read_bytes()).hexdigest(),
        baseline_python_tests=baseline["python_tests"],
        host_graphics_sanitizers_passed=True,
        sources={
            str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (
                ROOT / "controller.cpp",
                ROOT / "plugin.cpp",
                ROOT / "dashboard/renderer.cpp",
                ROOT / "source_mapping.hpp",
            )
        },
    )
    (OUT / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
