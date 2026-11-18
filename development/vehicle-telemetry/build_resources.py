#!/usr/bin/env python3
"""Assemble native HMI resources and a non-installing review bundle."""

import hashlib
import json
import math
import shutil
import struct
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parents[1]
DATE = (2026, 11, 11, 0, 0, 0)
RU = {
    1: "Параметры автомобиля",
    2: "Двигатель",
    3: "Наддув и топливо",
    4: "Электрика",
    5: "Движение",
    7: "АКПП",
    8: "Валы и передача",
}
EN = {
    1: "Vehicle parameters",
    2: "Engine",
    3: "Boost and fuel",
    4: "Electrical system",
    5: "Driving",
    7: "Transmission",
    8: "Shafts and gear",
}


def put(archive, name, body):
    entry = zipfile.ZipInfo(name, DATE)
    entry.create_system = 3
    entry.external_attr = 0o100644 << 16
    entry.compress_type = zipfile.ZIP_DEFLATED
    archive.writestr(entry, body)


def icon():
    """Own 63x54 gauge icon, supersampled; no firmware or external artwork."""
    scale = 4
    w, h = 63, 54
    pixels = bytearray()

    def line_distance(x, y):
        dx, dy = 13, -13
        t = max(0, min(1, ((x - 31) * dx + (y - 29) * dy) / (dx * dx + dy * dy)))
        return math.hypot(x - 31 - t * dx, y - 29 - t * dy)

    for py in range(h):
        pixels.append(0)
        for px in range(w):
            channels = [0, 0, 0, 0]
            for sy in range(scale):
                for sx in range(scale):
                    x = px + (sx + 0.5) / scale
                    y = py + (sy + 0.5) / scale
                    distance = math.hypot(x - 31, y - 27)
                    angle = math.degrees(math.atan2(y - 27, x - 31)) % 360
                    rgb = None
                    if abs(distance - 21) < 1.5 and not 45 < angle < 135:
                        rgb = (224, 233, 242)
                    if line_distance(x, y) < 1.3 or math.hypot(x - 31, y - 29) < 3:
                        rgb = (51, 160, 210)
                    if rgb:
                        for i, value in enumerate((*rgb, 255)):
                            channels[i] += value
            pixels.extend(round(v / (scale * scale)) for v in channels)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(pixels, 9))
        + chunk(b"IEND", b"")
    )


def build(candidate, folder):
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    evidence = json.loads((PROJECT / "tests/fixtures/vehicle-telemetry/vehicle-menu-146.json").read_text())
    placement = evidence["placement"]
    if (placement["insert_state"], placement["insert_after_focus"]) != (580, 2640):
        raise ValueError("unexpected native menu placement")
    for name in ("VehicleTelemetry.xml", "VehicleTelemetry.manifest"):
        shutil.copyfile(ROOT / name, folder / name)
    shutil.copyfile(candidate, folder / "libVehicleTelemetry.so")
    shutil.copyfile(ROOT / "vendor/kizapp/LICENSE", folder / "LICENSE")
    (folder / "NOTICE.txt").write_text(
        "VehicleTelemetry: GPL-3.0-or-later.\n"
        "Uses the unmodified profile_gate.hpp from kizapp/entrynav2-idrive6-android-auto,\n"
        "commit 5d7b6daabdf2ef2cb751d2030f44aa62910974ea (GPL-3.0-or-later).\n"
        "Native factory lifecycle follows the published src/native_app.cpp pattern.\n"
        "Source and provenance are in development/vehicle-telemetry/ beside this build.\n"
    )
    with zipfile.ZipFile(folder / "VehicleTelemetry_Texts.zip", "w") as archive:
        for language in ("ru", "en-UK", "en-US", "de", "fr-FR", "fr-CA", "es-ES", "es-US", "it", "pt"):
            words = RU if language == "ru" else EN
            body = (
                "100000="
                + ("Russian" if language == "ru" else "English")
                + "\n"
                + "".join(f"{key}={value}\n" for key, value in words.items())
            ).encode("utf-8")
            put(archive, language + ".txt", body)
    with zipfile.ZipFile(folder / "VehicleTelemetry_Images.zip", "w") as archive:
        put(archive, "1.png", icon())
    profile = dict(
        firmware="002_146_002",
        menu_placement=placement,
        installed=False,
        vehicle_tested=False,
        ui_format="native HMI XML and resource ZIPs",
        mode="offline review candidate, not installer",
    )
    (folder / "profile.json").write_text(json.dumps(profile, indent=2) + "\n")
    (folder / "README-RU.txt").write_text(
        "Нативное меню Параметры автомобиля — кандидат 146\n\n"
        "Путь: Состояние автомобиля → Параметры автомобиля.\n"
        "Разделы: Двигатель; Наддув и топливо; Электрика; Движение; АКПП → Валы и передача.\n"
        "Выбор — штатный контроллер iDrive; Back из раздела возвращает в главное меню плагина.\n"
        "Back из главного меню должен возвращать в штатный раздел через native entry point.\n"
        "Target state 580 / tyre-pressure anchor 2640 подтверждены по сохранённому HMI 146.\n"
        "Вставка, уникальность focus 32750, чтение датчиков и Back на физическом HU не проверены.\n"
        "Установщика нет. Не заменять установленный AA и не копировать link-only библиотеки BMW.\n"
        "Комплект содержит собственный DSO и ресурсы HMI; сайта и браузера для его работы нет.\n"
        "Опрос работает, пока открыты страницы плагина; число открытий не ограничено.\n"
        "DDE 0F1FF0 / EGS 0F21D0: опрос проверенных записей при открытом меню; после выхода сокеты закрываются.\n",
        encoding="utf-8",
    )
    files = sorted(p for p in folder.iterdir() if p.is_file() and p.name != "SHA256SUMS")
    (folder / "SHA256SUMS").write_text(
        "".join(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.name + "\n" for p in files)
    )
    bundle = folder.parent / "VehicleTelemetry-native-menu-146-candidate.zip"
    with zipfile.ZipFile(bundle, "w") as archive:
        for path in sorted(folder.iterdir()):
            if path.is_file():
                put(archive, "VehicleTelemetry/" + path.name, path.read_bytes())
    return dict(
        folder=str(folder),
        bundle=str(bundle),
        bundle_sha256=hashlib.sha256(bundle.read_bytes()).hexdigest(),
        files=[p.name for p in sorted(folder.iterdir()) if p.is_file()],
        placement=placement,
        installed=False,
    )


if __name__ == "__main__":
    print(json.dumps(build(ROOT / "build/libVehicleTelemetry.146-candidate.so", ROOT / "build/native-menu"), indent=2))
