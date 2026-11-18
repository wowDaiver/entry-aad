#!/usr/bin/env python3
"""Replay saved block responses through the native decoder and interface. No network I/O."""

import argparse
import hashlib
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from host_tools import compiler, default_font, freetype_flags  # noqa: E402

PROJECT = ROOT.parents[1]
EVIDENCE = PROJECT / "tests/fixtures/vehicle-telemetry/sensors-source-2026-11-17"
FONT = default_font()
EXE = ROOT / "build/mapping-replay"
CAPTURES = [
    EVIDENCE / name
    for name in (
        "panel-live-20261117-1602.json",
        "panel-followup-20261117-1610.json",
        "d83-source/live-d83-20261117.json",
        "d83-source/live-current-20261117.json",
        "d83-source/engine-load-check.json",
    )
]
CAPTURES.append(PROJECT / "tests/fixtures/vehicle-telemetry/panel-followup-2026-11-18/missing-sources-live.json")
CAPTURES += [
    PROJECT / "tests/fixtures/vehicle-telemetry/brake-regen-2026-11-18" / name
    for name in ("parked-brake-engaged.json", "parked-brake-released.json")
]


def build():
    EXE.parent.mkdir(exist_ok=True)
    subprocess.run(
        [
            compiler(),
            "-std=c++11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-misleading-indentation",
            "-fsanitize=address,undefined",
            str(ROOT / "dashboard/renderer.cpp"),
            str(ROOT / "dashboard/mapping_replay.cpp"),
            str(ROOT / "telemetry_model.cpp"),
            str(ROOT / "probe.cpp"),
            *freetype_flags(),
            "-o",
            str(EXE),
        ],
        check=True,
    )


def wire_record(row):
    """Use actual HSFZ reply bytes; never trust the capture's interpretation/value."""
    ecu, key = int(row["ecu"], 16), int(row["did"], 16)
    if not 0 <= ecu <= 255 or not 0 <= key <= 65535:
        raise ValueError("address outside bounds")
    request = bytes.fromhex(row["request_hex"])
    query = bytes((1, key & 255)) if key in (0x0104, 0x010D) else b"\x22" + key.to_bytes(2, "big")
    expected = bytes((0xF4, ecu)) + query
    if request != len(expected).to_bytes(4, "big") + b"\x00\x01" + expected:
        raise ValueError("capture request/address mismatch")
    frames = row["frames"]
    if len(frames) > 32:
        raise ValueError("too many frames")
    replies, total = [], 0
    for frame in frames:
        packet = bytes.fromhex(frame)
        total += len(packet)
        if total > 4096 or len(packet) < 6 or int.from_bytes(packet[:4], "big") != len(packet) - 6:
            raise ValueError("malformed HSFZ frame")
        if packet[4:6] != b"\x00\x01":  # ACK / alive frames are not measurements.
            continue
        if len(packet) < 9 or packet[6:8] != bytes((ecu, 0xF4)):
            raise ValueError("capture response/address mismatch")
        payload = packet[8:]
        positive = bytes((0x41, key & 255)) if query[0] == 1 else b"\x62" + query[1:]
        if payload.startswith(positive):
            data = payload[len(positive) :]
            if not data or len(data) > 16:
                raise ValueError("record size outside replay bounds")
            replies.append(("positive", data))
        elif len(payload) == 3 and payload[:2] == bytes((0x7F, query[0])):
            if payload[2] != 0x78:
                replies.append(("negative", payload[2:]))
        else:
            raise ValueError("response service/key mismatch")
    if len(replies) != 1:
        raise ValueError("expected one final diagnostic reply")
    status, data = replies[0]
    if row["status"] != status:
        raise ValueError("capture status differs from raw reply")
    if status == "positive" and (bytes.fromhex(row["data_hex"]) != data or row["length"] != len(data)):
        raise ValueError("capture data differs from raw reply")
    if status == "negative" and int(row["nrc"], 16) != data[0]:
        raise ValueError("capture NRC differs from raw reply")
    return ecu, key, status, data


def capture_events(path):
    capture = json.loads(path.read_text())
    if len(capture["results"]) > 128:
        raise ValueError("too many capture results")
    lines = []
    for row in capture["results"]:
        ecu, key, status, data = wire_record(row)
        if key == 0xF150:
            if status != "positive":
                raise ValueError("no positive ECU identity")
            lines.append(f"I {ecu:x} {data.hex()}")
        elif status == "positive":
            lines.append(f"D {ecu:x} {key:x} {data.hex()}")
        else:
            lines.append(f"N {ecu:x} {key:x}")
    return "\n".join(lines) + "\n", capture.get("time_utc")


def replay(events, png, age=0):
    result = subprocess.run(
        [str(EXE), str(FONT), str(png), str(age)], input=events, text=True, capture_output=True, check=True
    )
    return json.loads(result.stdout)


def verify_ui_bindings(report):
    plugin = ET.parse(ROOT / "VehicleTelemetry.xml").getroot().find("pluginApp")
    models = {int(n.attrib["id"]) for n in plugin.find("models")}
    states = {int(n.attrib["id"]): n for n in plugin.find("hmiStates")}
    seen_models, seen_slots = set(), set()
    for row in report["metrics"]:
        model, state, slot = row["hmi_model"], row["hmi_state"], row["panel_slot"]
        if model == 0:
            if row["source"] == "unidentified" or (slot is None and state != 0):
                raise ValueError("invalid auxiliary source binding")
            continue
        if model in seen_models or model not in models or state not in states:
            raise ValueError("missing or duplicate native model/state binding")
        if not any(n.get("model") == str(model) for n in states[state].iter("label")):
            raise ValueError("metric is not displayed on its declared text page")
        if slot is not None:
            if slot in seen_slots:
                raise ValueError("two sources overwrite one panel slot")
            seen_slots.add(slot)
        seen_models.add(model)


def source_label(row):
    if row["source"] == "cds":
        return f"CDS `{row['cds_property']}` → `{row['path']}`"
    if row["source"] == "uds":
        return f"ECU `{row['ecu']:02X}` / `22 {row['key']:04X}`"
    if row["source"] == "obd01":
        return f"ECU `{row['ecu']:02X}` / `01 {row['key'] & 255:02X}`"
    return "Источник не определён"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--capture", type=Path, action="append", help="Saved JSON capture; each file is replayed separately"
    )
    parser.add_argument("--out", type=Path, default=ROOT / "build/offline-mapping")
    parser.add_argument(
        "--age", type=float, default=0, help="Seconds elapsed on the replay clock, not real measurement age"
    )
    args = parser.parse_args()
    if not 0 <= args.age <= 86400:
        parser.error("--age must be finite and within 0..86400")
    build()
    args.out.mkdir(parents=True, exist_ok=True)
    reports, names = [], set()
    for capture in args.capture or CAPTURES:
        stem = capture.stem
        if not re.fullmatch(r"[a-zA-Z0-9_-]+", stem) or stem in names:
            raise ValueError("duplicate or unsafe capture output name")
        names.add(stem)
        events, time = capture_events(capture)
        report = replay(events, args.out / (stem + ".png"), args.age)
        verify_ui_bindings(report)
        (args.out / (stem + ".events")).write_text(events)
        report.update(
            captured_at=time,
            capture_sha256=hashlib.sha256(capture.read_bytes()).hexdigest(),
            synthetic_values=False,
            vehicle_contacted=False,
            per_record_timestamps_available=False,
            time_basis=(
                "Capture group replayed at t=0; individual measurement times are unknow" "n. Files are never merged."
            ),
            png=stem + ".png",
        )
        (args.out / (stem + ".json")).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        reports.append(report)
    mapping = []
    for row in reports[0]["metrics"]:
        binding = {
            k: row[k]
            for k in ("name", "source", "ecu", "key", "path", "hmi_model", "hmi_state", "panel_slot", "cds_property")
        }
        binding["observed_units"] = sorted(
            {
                r["unit"]
                for report in reports
                for r in report["metrics"]
                if r["name"] == row["name"] and r["value"] is not None
            }
        )
        mapping.append(binding)
    summary = dict(
        kind="offline-source-map",
        vehicle_contacted=False,
        snapshots=len(reports),
        metrics=mapping,
        panel_unbound=[],
        native_sources={
            str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (
                ROOT / "source_mapping.hpp",
                ROOT / "source_input.hpp",
                ROOT / "telemetry_model.cpp",
                ROOT / "dashboard/panel_data.hpp",
                ROOT / "dashboard/renderer.cpp",
                ROOT / "dashboard/recording.hpp",
            )
        },
        derived={
            "power": dict(inputs=["torque", "rpm"], requires="Both valid, time skew <=2 seconds", unit="metric_hp")
        },
    )
    (args.out / "source-map.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    lines = [
        "# Карта источников → интерфейс",
        "",
        (
            "Сгенерировано из нативной карты source_mapping.hpp. Сырые ответы декод"
            "ируются тем же Model, что использует плагин."
        ),
        "",
        "| Метрика | Источник | Модель RHMI / страница | Поле приборов |",
        "| --- | --- | --- | --- |",
    ]
    for row in mapping:
        target = f"{row['hmi_model']} / {row['hmi_state']}" if row["hmi_model"] else "Приборы / служебный источник"
        slot = row["panel_slot"] or "Подробный раздел / выбор источника"
        lines.append(f"| {row['name']} | {source_label(row)} | {target} | {slot} |")
    lines += [
        "",
        (
            "Момент: DDE 48A5; заряд АКБ: DDE 43C2 с резервным CDS; ручник: EGS DA3"
            "1. Рекуперация: прирост отдельного счётчика DDE 40C0 за интервал до 3 "
            "с; одиночный архив не определяет активность. Мощность скрыта без свежи"
            "х момента и оборотов."
        ),
        "",
        (
            "Это архивные ответы отдельных проходов. У них есть время группы, но не"
            "т времени каждого измерения: файлы не объединяются. Свежесть оценивает"
            "ся только на часах воспроизведения. Числа не считаются текущими показа"
            "ниями автомобиля."
        ),
        "",
        (
            "Идентичности: DDE 12 / 0F1FF0, EGS 18 / 0F21D0. Данные другого профиля"
            ", неподключённые DID и неправильные длины не заполняют интерфейс."
        ),
        "",
        (
            "Обороты 600A остаются отдельным полем АКПП и не заменяют автоматически"
            " CDS-обороты двигателя. 586F не используется как давление масла или So"
            "C. Ток не определяет рекуперацию; P не определяет ручник."
        ),
        "",
        "Снимки:",
    ]
    for report in reports:
        lines.append(f"- [{report['png']}]({report['png']}) — {report['captured_at']} (время группы)")
    (args.out / "SOURCE-MAP-RU.md").write_text("\n".join(lines) + "\n")
    print(
        json.dumps(
            dict(snapshots=len(reports), metrics=len(mapping), out=str(args.out), vehicle_contacted=False),
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    main()
