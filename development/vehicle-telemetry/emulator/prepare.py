#!/usr/bin/env python3
"""Prepare a separate NIC-less local146 VM. Never invokes the car/offline SSH kit."""

import argparse
import gzip
import hashlib
import importlib.util
import json
import plistlib
import subprocess
import uuid
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
QEMU = ROOT.parents[2] / "entrynav2-qemu"
parser = argparse.ArgumentParser()
parser.add_argument("--refresh-initrd", action="store_true")
parser.add_argument("--build-only", action="store_true")
parser.add_argument("--entry", choices=("media", "vehicle-status"), default="vehicle-status")
parser.add_argument("--dashboard", action="store_true", help="Emulator-only graphic dashboard; requires --demo")
parser.add_argument(
    "--demo", action="store_true", help="Explicitly labelled emulator-only battery/temperature values; no CDS reads"
)
args = parser.parse_args()
if args.dashboard and not args.demo:
    parser.error("--dashboard requires --demo; no vehicle transport in graphics demo")
out = HERE / "build"
out.mkdir(exist_ok=True)
base = ROOT / "build"
compiler = QEMU / "lab/qemu-runtime/software/bin/cxx"
objects = [base / (name + ".o") for name in ("probe", "native_cds", "telemetry_model", "egs_reader")]
defines = ["-DTELEMETRY_GRAPHICS"] if args.dashboard else []
if args.dashboard:
    renderer = out / "dashboard-renderer.o"
    subprocess.run(
        [
            str(compiler),
            "-O2",
            "-fPIC",
            "-fvisibility=hidden",
            "-I/opt/X11/include/freetype2",
            "-c",
            str(ROOT / "dashboard/renderer.cpp"),
            "-o",
            str(renderer),
        ],
        check=True,
    )
    objects.append(renderer)
subprocess.run(
    [
        str(compiler),
        "-O2",
        "-fPIC",
        "-fvisibility=hidden",
        "-fvisibility-inlines-hidden",
        "-DTELEMETRY_EMULATOR_ONLY",
        *defines,
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unused-command-line-argument",
        "-c",
        str(ROOT / "controller.cpp"),
        "-o",
        str(out / "controller.o"),
    ],
    check=True,
)
objects.append(out / "controller.o")
subprocess.run(
    [
        str(compiler),
        "-O2",
        "-fPIC",
        "-fvisibility=hidden",
        "-fvisibility-inlines-hidden",
        "-DTELEMETRY_EMULATOR_ONLY",
        *defines,
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unused-command-line-argument",
        "-I" + str(base),
        "-c",
        str(ROOT / "plugin.cpp"),
        "-o",
        str(out / "plugin.o"),
    ],
    check=True,
)
candidate = out / "libVehicleTelemetry.emulator.so"
subprocess.run(
    [
        str(compiler),
        "-shared",
        "-nostdlib++",
        "-Wl,-soname,libVehicleTelemetry.so",
        *map(str, objects),
        str(out / "plugin.o"),
        "-L" + str(base / "link-only"),
        "-Wl,--allow-shlib-undefined",
        "-l:libAppFramework.A.so",
        "-l:libCoreFramework.A.so",
        "-l:libstdc++.so.6.0.17",
        "-Wl,--version-script=" + str(base / "exports.map"),
        "-ldl",
        "-lrt",
        *(
            [
                "-L" + str(QEMU / "lab/owner146-capture/files/opt/hmi/lib"),
                "-l:libfreetype2.so",
                "-L" + str(QEMU / "lab/vendor/qemu-lab/system/artifacts/native-wayland-build/sysroot/lib"),
                "-l:libz.so.1.2.8",
            ]
            if args.dashboard
            else []
        ),
        "-o",
        str(candidate),
    ],
    check=True,
)
menu = ET.parse(base / "native-menu/VehicleTelemetry.xml")
if args.entry == "media":
    plugin = menu.getroot().find("pluginApp")
    plugin.set("applicationType", "Multimedia")
    for key in ("insertStateID", "insertAfterFocusID", "entryButtonFocusID"):
        plugin.attrib.pop(key, None)
if args.dashboard:
    plugin = menu.getroot().find("pluginApp")
    ET.SubElement(plugin.find("actions"), "hmiAction", id="44", target="15")
    ET.SubElement(plugin.find("models"), "raImageModel", id="120")
    ET.SubElement(plugin.find("models"), "textIdModel", id="24", textId="6")
    # Keep known-good text pages, add a separate graphic page at the top.
    components = plugin.find("hmiStates/hmiState").find("components")
    components.insert(0, ET.Element("button", id="205", action="44", model="24", text="Приборы"))
    page = ET.SubElement(plugin.find("hmiStates"), "hmiState", id="15", parentId="10", text="Панель приборов · тест")
    image = ET.SubElement(ET.SubElement(page, "components"), "image", id="290", model="120")
    props = ET.SubElement(image, "properties")
    # The 146 A4A layout uses 1440x540 coordinates on this 1280x480 display.
    # Expand below the native header and reclaim the list's leading gutter.
    for key, val in [(2, "false"), (3, "true"), (9, "1350"), (10, "477"), (20, "-90"), (21, "0")]:
        ET.SubElement(props, "property", id=str(key), value=val)
menu.write(out / "VehicleTelemetry.xml", encoding="utf-8", xml_declaration=True)
ids = [node.get("id") for node in menu.getroot().iter() if node.get("id") is not None and node.tag != "property"]
if len(ids) != len(set(ids)):
    raise RuntimeError("native resource IDs collide")
if args.dashboard:
    with zipfile.ZipFile(base / "native-menu/VehicleTelemetry_Texts.zip") as src, zipfile.ZipFile(
        out / "VehicleTelemetry_Texts.zip", "w", zipfile.ZIP_DEFLATED
    ) as target:
        for name in src.namelist():
            text = src.read(name).decode("utf-8") + "6=" + ("Приборы" if name == "ru.txt" else "Dashboard") + "\n"
            target.writestr(name, text.encode("utf-8"))
if args.build_only:
    print(candidate)
    raise SystemExit(0)
spec = importlib.util.spec_from_file_location("cpio", QEMU / "shader-lab/scripts/build.py")
cpio = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cpio)
entries = {}


def add(name, body, mode=0o100644):
    for parent in reversed(Path(name).parents):
        if str(parent) != ".":
            entries.setdefault(str(parent), (b"", 0o40755))
    entries[name] = (body, mode)


add("qemu-integration/telemetry/VehicleTelemetry/libVehicleTelemetry.so", candidate.read_bytes(), 0o100755)
for name in (
    "VehicleTelemetry.xml",
    "VehicleTelemetry.manifest",
    "VehicleTelemetry_Texts.zip",
    "VehicleTelemetry_Images.zip",
):
    source = (
        out / name
        if name == "VehicleTelemetry.xml" or args.dashboard and name == "VehicleTelemetry_Texts.zip"
        else base / "native-menu" / name
    )
    add("qemu-integration/telemetry/VehicleTelemetry/" + name, source.read_bytes())
guest = (
    (QEMU / "runtime-lab/guest-init").read_text().replace("INTEGRATION_BEGIN_FULL156", "INTEGRATION_BEGIN_TELEMETRY146")
)
guest = guest.replace(
    "export DLT_LOCAL_PRINT_MODE=FORCE_OFF",
    (
        "export DLT_LOCAL_PRINT_MODE=FORCE_OFF\nexport OWNER146_LANGUAGE=17\nexpo"
        "rt VEHICLE_TELEMETRY_EMULATOR=isolated-owner146"
    ),
)
if args.demo:
    guest = guest.replace(
        "export VEHICLE_TELEMETRY_EMULATOR=isolated-owner146",
        "export VEHICLE_TELEMETRY_EMULATOR=isolated-owner146\nexport VEHICLE_TELEMETRY_DEMO=battery-and-temperatures",
    )
guest = guest.replace(
    "LD_PRELOAD=/qemu-integration/libframe-timing.so:/qemu-integration/libshaderlab.so:",
    (
        "LD_PRELOAD=/qemu-integration/libowner146-language.so:/qemu-integration"
        "/libframe-timing.so:/qemu-integration/libshaderlab.so:"
    ),
)
anchor = "echo INTEGRATION_HMI_BEGIN"
assert guest.count(anchor) == 1
guest = guest.replace(
    anchor,
    (
        "mkdir -p /opt/hmi/data/ro/Plugins\ncp -a /qemu-integration/telemetry/Ve"
        "hicleTelemetry /opt/hmi/data/ro/Plugins/\necho TELEMETRY146_NATIVE_RESO"
        "URCES_INSTALLED\n"
    )
    + anchor,
)
add("qemu-integration/init", guest.encode(), 0o100755)
archive = b"".join(
    cpio.newc(n, body, mode, 850000 + i) for i, (n, (body, mode)) in enumerate(entries.items())
) + cpio.newc("TRAILER!!!", b"", 0, 859999)
vm = QEMU / "runtime/EntryNav2-Telemetry146.utm"
data = vm / "Data"
original = QEMU / "runtime/EntryNav2-Owner146.utm"
uid = str(uuid.uuid5(uuid.NAMESPACE_DNS, "entrynav2-owner146-vehicle-telemetry"))
if vm.exists() and not args.refresh_initrd:
    raise RuntimeError("VM exists; use --refresh-initrd only with this VM stopped")
data.mkdir(parents=True, exist_ok=True)
if args.refresh_initrd:
    config = plistlib.loads((vm / "config.plist").read_bytes())
    assert config["Information"]["UUID"] == uid and not config["Network"]
else:
    for name in ("firmware.ext4", "vmlinuz"):
        subprocess.run(["cp", "-c", str(original / "Data" / name), str(data / name)], check=True)
    config = plistlib.loads((original / "config.plist").read_bytes())
    config["Information"].update(
        Name="EntryNav2 146 — Параметры авто",
        UUID=uid,
        Notes=(
            "Isolated local native telemetry-menu test. Exact146 HMI; remaining OS1"
            "56. No NIC/USB. No real vehicle data."
        ),
    )
    config["QEMU"]["AdditionalArguments"] = [
        str(x).replace("port=17702", "port=17703").replace(str(original / "Data"), str(data))
        for x in config["QEMU"]["AdditionalArguments"]
    ]
    config["Network"] = []
    config["Input"].update(UsbBusSupport="Disabled", UsbSharing=False, MaximumUsbShare=0)
    config["Sharing"].update(ClipboardSharing=False, DirectoryShareMode="None")
    (vm / "config.plist").write_bytes(plistlib.dumps(config))
temp = data / "initrd-next.gz"
subprocess.run(["cp", "-c", str(original / "Data/initrd.gz"), str(temp)], check=True)
with temp.open("ab") as target:
    target.write(gzip.compress(archive, compresslevel=3, mtime=0))
temp.replace(data / "initrd.gz")
(HERE / "start-menu.command").write_text(
    '#!/bin/zsh\nset -eu\ncd "${0:A:h:h:h:h:h}/entrynav2-qemu"\nexec runtime/UTM.app/Contents/MacOS/utmctl start '
    + uid.upper()
    + "\n"
)
(HERE / "start-menu.command").chmod(0o755)
report = dict(
    vm=str(vm),
    uuid=uid,
    serial="127.0.0.1:17703",
    network_adapters=0,
    usb_forwarding=False,
    vehicle_contacted=False,
    vehicle_files_modified=False,
    offline_kit_modified=False,
    variant="TELEMETRY_EMULATOR_ONLY",
    native_plugin_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
    entry=args.entry,
    vehicle_status_activation_verified=False,
    graphic_dashboard=args.dashboard,
    synthetic_telemetry=args.demo,
    synthetic_label="(тест)" if args.demo else None,
    base_vm_modified=False,
)
(out / "prepare.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
