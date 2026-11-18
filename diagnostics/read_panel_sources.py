#!/usr/bin/env python3
"""One read-only source check, never a poller or installer. Default: offline plan.

Fixed ECU 12/18, UDS 22 only. Eight requests maximum, one per ECU/DID,
3 seconds and 32 frames per request. No sessions, resets or security access.
Catalog layouts remain provisional even after a syntactically positive reply.
"""

import argparse
import datetime
import ipaddress
import json
import socket
import struct
import subprocess
import time
from pathlib import Path

from enet import bound_socket, interface_info

SOURCE = "https://github.com/OBDb/BMW-X3/blob/395d22f58cc91854d4459302fad6c5a6f6c44af2/signalsets/v3/default.json"
RECORDS = {
    0x12: {0xF150: "sgbd", 0x586F: "oil_pressure", 0x402B: "battery_record"},
    0x18: {
        0xF150: "sgbd",
        0x6000: "gearbox_temperature",
        0xDA2E: "selector_and_mode",
        0xDA22: "converter_lock",
        0xDA2A: "shaft_speeds",
    },
}
# Separate follow-up, not added to the initial eight-request check. EGS native
# schemas: archived idrive7-apps/apps/zf8hp/catalog.js (GSZFBCE revision 15.000).
FOLLOWUP = {
    0x12: {0xF150: "sgbd", 0xF197: "system_name"},
    0x18: {
        0xF150: "sgbd",
        0x6008: "native_output_rpm",
        0x6009: "native_turbine_rpm",
        0x600A: "native_engine_rpm",
        0x600C: "native_current_gear",
    },
}
D83_SOURCE = (
    "https://github.com/emdzej/ediabasx-docs-sgbd/blob/b644de8fbfbb4b207f57"
    "794e3c7894dc1dc58627/docs/sgbd/D83BX7C0.md#table-sg-funktionen"
)
# Explicit source-backed reads, guarded by the actual F150 profile 0F1FF0.
# No generic catalog oil/IBS records are part of this plan.
D83 = {
    0x12: {
        0xF150: "sgbd",
        0x4286: "battery_current_ibs",
        0x428C: "battery_temperature_ibs",
        0x5866: "battery_voltage_available",
        0x5868: "battery_temperature_available",
        0x42B9: "oil_pressure_corrected_relative",
        0x42BC: "oil_pressure_actual",
        0x4B5E: "oil_pressure_sensor",
    }
}
D83_CURRENT = {
    0x12: {
        0xF150: "sgbd",
        0x43C0: "battery_current_energy_management",
        0x42EE: "storage_current_available",
        0x42EB: "battery_terminal_voltage",
        0x5868: "battery_temperature_available",
        0x42B9: "oil_pressure_corrected_relative",
    }
}
D83_FORMATS = {
    0x4286: (2, 0.08, -200, "A"),
    0x428C: (2, 0.00923705, -50, "C"),
    0x5866: (2, 0.001, 0, "V"),
    0x5868: (1, 1, -126, "C"),
    0x42B9: (2, 1, 0, "hPa"),
    0x42BC: (2, 1, 0, "hPa"),
    0x4B5E: (2, 1, 0, "hPa"),
    0x43C0: (2, 0.05, -1638.35, "A"),
    0x42EE: (2, 0.05, 0, "A"),
    0x42EB: (2, 0.001, 0, "V"),
}
ENGINE_PROBE = {
    0x12: {
        0xF150: "sgbd",
        0xF404: "calculated_engine_load_uds",
        0x0104: "calculated_engine_load_obd",
        0x010D: "vehicle_speed_obd",
    }
}
PANEL_MISSING = {
    0x12: {
        0xF150: "sgbd",
        0x43C2: "battery_soc_energy_management",
        0x42B1: "storage_soc_crosscheck",
        0x48A5: "actual_engine_torque",
        0x48A6: "actual_clutch_torque_crosscheck",
    },
    0x18: {0xF150: "sgbd", 0xDA27: "gear_lever_position", 0xDA2E: "selector_and_mode"},
}
PANEL_FORMATS = {
    0x43C2: (1, 1, -127, "%"),
    0x42B1: (1, 1, -128, "%"),
    0x48A5: (2, 0.11444266, -2500, "Nm"),
    0x48A6: (2, 0.11444266, -2500, "Nm"),
}
GSZ_SOURCE = (
    "https://github.com/emdzej/ediabasx-docs-sgbd/blob/b644de8fbfbb4b207f57"
    "794e3c7894dc1dc58627/docs/sgbd/GSZFB1.md#table-tab-feststellbremse"
)
# Discovery only: do not enable a dashboard status from an unknown generator
# enum, a cumulative counter, charging current, or the P selector position.
BRAKE_REGEN = {
    0x12: {
        0xF150: "sgbd",
        0x46B4: "parking_brake_lamp_raw",
        0x43BF: "generator_state_raw",
        0x586D: "producer_status_raw",
        0x42A1: "recuperated_charge_candidate",
        0x40C0: "igr_charge_counters_candidate",
    },
    0x18: {0xF150: "sgbd", 0xDA31: "parking_brake_status"},
}
PARKING_BRAKE = {
    0x12: {0xF150: "sgbd", 0x40C0: "igr_charge_counters_candidate"},
    0x18: {0xF150: "sgbd", 0xDA31: "parking_brake_status"},
}
RECORD_SETS = (RECORDS, FOLLOWUP, D83, D83_CURRENT, ENGINE_PROBE, PANEL_MISSING, BRAKE_REGEN, PARKING_BRAKE)
LENGTHS = {
    (0x12, 0x586F): 2,
    (0x12, 0x402B): 6,
    (0x18, 0x6000): 1,
    (0x18, 0xDA2E): 2,
    (0x18, 0xDA22): 2,
    (0x18, 0xDA2A): 4,
}
STOP = {"incomplete", "transport_rejected", "frame_limit"}


def decode(ecu, did, data):
    """Offline candidate interpretation, no inference about ECU compatibility."""
    if ecu == 0x18 and did == 0xDA31:
        if len(data) != 2:
            return {"status": "layout_mismatch", "expected_length": 2, "actual_length": len(data)}
        raw = int.from_bytes(data, "big")
        return {
            "status": "sgbd_candidate_only",
            "source": GSZ_SOURCE,
            "fields": {
                "parking_brake_status": {
                    "raw": raw,
                    "value": raw if raw in (0, 1) else None,
                    "valid": raw in (0, 1),
                    "label": {0: "released", 1: "engaged"}.get(raw),
                }
            },
        }
    if ecu == 0x12 and did in (0x46B4, 0x43BF, 0x586D, 0x42A1, 0x40C0):
        size = {0x46B4: 1, 0x43BF: 1, 0x586D: 1, 0x42A1: 2, 0x40C0: 16}[did]
        if len(data) != size:
            return {"status": "layout_mismatch", "expected_length": size, "actual_length": len(data)}
        if did == 0x40C0:
            fields = {
                name: {
                    "raw": int.from_bytes(data[at : at + 4], "big"),
                    "value": int.from_bytes(data[at : at + 4], "big") / divisor,
                    "unit": unit,
                }
                for at, name, divisor, unit in (
                    (0, "recuperation_charge_since_battery_change", 10, "As"),
                    (4, "balancehold_charge_since_battery_change", 10, "As"),
                    (8, "discharge_charge_since_battery_change", 10, "As"),
                    (12, "recuperation_charge_bucket", 16777216, "Ah"),
                )
            }
            fields["balancehold_charge_since_battery_change"]["raw"] = int.from_bytes(data[4:8], "big", signed=True)
            fields["balancehold_charge_since_battery_change"]["value"] = (
                fields["balancehold_charge_since_battery_change"]["raw"] / 10
            )
        else:
            raw = int.from_bytes(data, "big")
            fields = {
                BRAKE_REGEN[ecu][did]: {
                    "raw": raw,
                    "value": raw * 0.01820401 if did == 0x42A1 else raw,
                    "unit": "Ah" if did == 0x42A1 else "raw_enum",
                }
            }
        return {
            "status": "sgbd_candidate_only",
            "source": D83_SOURCE,
            "fields": fields,
            "active_recuperation": None,
            "enum_semantics_verified": False,
            "sentinel_semantics_verified": False,
        }
    if ecu == 0x12 and did in PANEL_FORMATS:
        size, multiplier, offset, unit = PANEL_FORMATS[did]
        if len(data) != size:
            return {"status": "layout_mismatch", "expected_length": size, "actual_length": len(data)}
        raw = int.from_bytes(data, "big")
        return {
            "status": "sgbd_candidate_only",
            "source": D83_SOURCE,
            "fields": {PANEL_MISSING[ecu][did]: {"raw": raw, "value": raw * multiplier + offset, "unit": unit}},
            "sentinel_semantics_verified": False,
        }
    if ecu == 0x18 and did == 0xDA27:
        if len(data) != 2:
            return {"status": "layout_mismatch", "expected_length": 2, "actual_length": len(data)}
        return {
            "status": "sgbd_candidate_only",
            "fields": {
                "gear_lever_position": {
                    "raw": data[0],
                    "bus_error": data[1],
                    "valid": data[0] <= 7 and data[1] == 0,
                    "label": {0: "rest", 1: "P-button", 2: "R", 3: "N", 4: "D", 5: "M-", 6: "M+", 7: "M/S"}.get(
                        data[0]
                    ),
                }
            },
        }
    if ecu == 0x12 and did in (0xF404, 0x0104, 0x010D):
        if len(data) != 1:
            return {"status": "layout_mismatch", "expected_length": 1, "actual_length": len(data)}
        return {
            "status": "candidate_only",
            "fields": {
                ENGINE_PROBE[ecu][did]: {
                    "value": data[0] if did == 0x010D else data[0] * 100 / 255,
                    "unit": "km/h" if did == 0x010D else "%",
                    "raw": data[0],
                }
            },
        }
    if ecu == 0x12 and did in D83_FORMATS:
        size, multiplier, offset, unit = D83_FORMATS[did]
        if len(data) != size:
            return {"status": "layout_mismatch", "expected_length": size, "actual_length": len(data)}
        raw = int.from_bytes(data, "big")
        return {
            "status": "sgbd_candidate_only",
            "source": D83_SOURCE,
            "fields": {
                next(records[ecu][did] for records in RECORD_SETS if did in records.get(ecu, {})): {
                    "raw": raw,
                    "value": raw * multiplier + offset,
                    "unit": unit,
                }
            },
            "sentinel_semantics_verified": False,
        }
    if ecu == 0x12 and did == 0xF197 and 1 <= len(data) <= 128:
        return {"status": "identity", "system_name_ascii": data.decode("ascii", errors="replace").rstrip("\x00 ")}
    if ecu == 0x18 and did in (0x6008, 0x6009, 0x600A, 0x600C) and len(data) == 1:
        name = FOLLOWUP[ecu][did]
        return {
            "status": "candidate_only",
            "fields": {
                name: {
                    "value": data[0] if did == 0x600C else data[0] * 32,
                    "unit": "raw_gear_code" if did == 0x600C else "rpm",
                    "valid": True,
                }
            },
        }
    expected = 3 if did == 0xF150 and ecu in RECORDS else LENGTHS.get((ecu, did))
    if expected is None or len(data) != expected:
        return {"status": "layout_mismatch", "expected_length": expected, "actual_length": len(data)}
    fields = {}

    def add(name, value, unit, valid=True):
        fields[name] = {"value": value if valid else None, "unit": unit, "valid": valid}

    if did == 0xF150:
        return {"status": "identity", "sgbd_hex": data.hex()}
    if did == 0x586F:
        add("oil_pressure", int.from_bytes(data, "big") / 1000, "bar")
    elif did == 0x402B:
        temp, voltage, current = struct.unpack("!hHH", data)
        add("battery_temperature", temp / 10, "C", data != b"\xff" * 6 and -600 <= temp <= 1250)
        add("voltage", voltage / 4000 + 6, "V", voltage != 65535)
        add("battery_current", current / 12.5 - 200, "A", current != 65535)
    elif did == 0x6000:
        add("gearbox_temperature", data[0] - 40, "C", data[0] <= 245)
    elif did == 0xDA2E:
        selector, mode = data
        add("gear_selector", {0: "P", 1: "R", 2: "N"}.get(selector, "D"), "enum", selector <= 9 or selector == 16)
        add("gear_mode", {0: "P", 1: "R", 2: "N", 3: "D", 4: "M"}.get(mode), "enum", mode <= 4)
    elif did == 0xDA22:
        state = {
            0: "open",
            1: "controlled_low",
            2: "controlled_high",
            3: "closed_low",
            4: "closed_high",
            8: "transition_open",
        }.get(data[1])
        add("converter_lock", state, "enum", state is not None)
    elif did == 0xDA2A:
        converter, output = struct.unpack("!hh", data)
        add("converter_rpm", converter, "rpm")
        add("output_shaft_rpm", output, "rpm")
    return {"status": "candidate_only", "fields": fields}


def request_frame(ecu, did):
    if not any(did in records.get(ecu, {}) for records in RECORD_SETS):
        raise ValueError("ECU/DID outside the fixed source-check allowlist")
    if did in (0x0104, 0x010D):
        return struct.pack("!IHBBBB", 4, 1, 0xF4, ecu, 1, did & 255)
    return struct.pack("!IHBBBH", 5, 1, 0xF4, ecu, 0x22, did)


def recv_exact(s, size, deadline):
    data = bytearray()
    while len(data) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Absolute response deadline reached")
        s.settimeout(remaining)
        chunk = s.recv(size - len(data))
        if not chunk:
            raise EOFError("Gateway closed connection")
        data.extend(chunk)
    return bytes(data)


def read_one(s, ecu, did):
    request = request_frame(ecu, did)
    result = {
        "ecu": f"0x{ecu:02x}",
        "did": f"0x{did:04x}",
        "name": next(records[ecu][did] for records in RECORD_SETS if did in records.get(ecu, {})),
        "request_hex": request.hex(),
        "frames": [],
    }
    deadline = time.monotonic() + 3
    service = 1 if did in (0x0104, 0x010D) else 0x22
    prefix = bytes((0x41, did & 255)) if service == 1 else b"\x62" + did.to_bytes(2, "big")
    total = 0
    try:
        s.settimeout(3)
        s.sendall(request)
        for _ in range(32):
            header = recv_exact(s, 6, deadline)
            size, control = struct.unpack("!IH", header)
            total += 6 + size
            if size > 512 or total > 4096:
                raise ValueError("HSFZ frame/evidence size limit reached")
            body = recv_exact(s, size, deadline)
            result["frames"].append((header + body).hex())
            if control == 0x12:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("Absolute alive ACK deadline reached")
                s.settimeout(remaining)
                s.sendall(bytes.fromhex("00000002001300f4"))
                continue
            if control >= 0x40:
                result.update(status="transport_rejected", control=control)
                return result
            if control != 1 or body[:2] != bytes((ecu, 0xF4)):
                continue
            uds = body[2:]
            if uds.startswith(prefix):
                data = uds[len(prefix) :]
                result.update(
                    status="positive", data_hex=data.hex(), length=len(data), interpretation=decode(ecu, did, data)
                )
                return result
            if len(uds) == 3 and uds[:2] == bytes((0x7F, service)):
                if uds[2] == 0x78:
                    continue  # responsePending does not extend the deadline
                result.update(status="negative", nrc=f"0x{uds[2]:02x}")
                return result
        result["status"] = "frame_limit"
    except (OSError, EOFError, ValueError) as error:
        result.update(status="incomplete", error=str(error))
    return result


def require_wired(interface):
    inventory = subprocess.run(
        ["/usr/sbin/networksetup", "-listallhardwareports"], check=True, capture_output=True, text=True
    ).stdout
    matches = [block for block in inventory.split("\n\n") if f"Device: {interface}\n" in block + "\n"]
    if len(matches) != 1 or any(name in matches[0] for name in ("Wi-Fi", "AirPort", "Bluetooth")):
        raise ValueError("Explicitly selected ENET interface must be a wired hardware port")


def collect(s, records_by_ecu=RECORDS, required_identity=None):
    results = []
    for ecu, records in records_by_ecu.items():
        identity = read_one(s, ecu, 0xF150)
        results.append(identity)
        if identity["status"] in STOP:
            break
        if identity["status"] != "positive" or identity["length"] != 3:
            identity["candidates_skipped"] = "No complete SGBD identity"
            continue
        if required_identity and identity["data_hex"] != required_identity.get(ecu):
            identity["candidates_skipped"] = "ECU profile differs from the source-check profile"
            continue
        if ecu == 0x18 and identity["data_hex"] != "0f21d0":
            identity["candidates_skipped"] = "EGS variant differs from the archived 0f21d0 profile"
            continue
        for did in records:
            if did == 0xF150:
                continue
            time.sleep(0.15)
            result = read_one(s, ecu, did)
            results.append(result)
            if result["status"] in STOP:
                return results
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="explicit live one-shot check; default has no I/O")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--followup", action="store_true", help="separate fixed identity/native EGS cross-check, 7 requests max"
    )
    mode.add_argument("--d83", action="store_true", help="source-backed D83BX7C0 reads, exact 0F1FF0 identity required")
    mode.add_argument(
        "--d83-current",
        action="store_true",
        help="six fixed alternate battery-current/cross-check reads, exact 0F1FF0 required",
    )
    mode.add_argument(
        "--engine-load", action="store_true", help="fixed read-only load/speed probe: F150,22F404,0104,010D"
    )
    mode.add_argument(
        "--panel-missing",
        action="store_true",
        help="eight fixed reads: SoC, torque, lever position and exact ECU identities",
    )
    mode.add_argument(
        "--brake-regen",
        action="store_true",
        help="eight fixed reads: brake, generator states and IGR charge counters; discovery only",
    )
    mode.add_argument(
        "--parking-brake",
        action="store_true",
        help="four fixed reads: EGS brake status and DDE IGR counter cross-check with exact identities",
    )
    parser.add_argument("--interface")
    parser.add_argument("--gateway", help="observed link-local ENET gateway, not HU SSH address")
    parser.add_argument("--stationary", action="store_true")
    parser.add_argument(
        "--conditions", choices=("engine-running", "ignition-on", "hu-only", "bench", "stationary-engine-unknown")
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--brake-state",
        choices=("engaged", "released", "unknown"),
        default="unknown",
        help="owner-reported state, evidence label only; never operates the brake",
    )
    args = parser.parse_args(argv)
    selected = (
        BRAKE_REGEN
        if args.brake_regen
        else PARKING_BRAKE
        if args.parking_brake
        else PANEL_MISSING
        if args.panel_missing
        else ENGINE_PROBE
        if args.engine_load
        else D83_CURRENT
        if args.d83_current
        else D83
        if args.d83
        else FOLLOWUP
        if args.followup
        else RECORDS
    )
    required_identity = (
        {0x12: "0f1ff0", 0x18: "0f21d0"}
        if args.panel_missing or args.brake_regen or args.parking_brake
        else {0x12: "0f1ff0"}
        if args.d83 or args.d83_current or args.engine_load
        else None
    )
    plan = {
        "mode": "offline-plan",
        "service": "0x22 and fixed OBD01 PID04/0D reads" if args.engine_load else "0x22 only",
        "catalog": D83_SOURCE if required_identity else SOURCE,
        "required_identity": required_identity,
        "requests": [
            {"ecu": f"0x{ecu:02x}", "did": f"0x{did:04x}", "name": name}
            for ecu, records in selected.items()
            for did, name in records.items()
        ],
        "max_requests": sum(len(records) for records in selected.values()),
        "request_deadline_seconds": 3,
        "limits": "No retries; 32 frames/4096 bytes per request; stop on transport failure",
        "production_polling_enabled": False,
        "vehicle_compatibility_confirmed": False,
    }
    if args.brake_regen or args.parking_brake:
        plan.update(
            catalog={"dde": D83_SOURCE, "egs": GSZ_SOURCE},
            brake_state_reported=args.brake_state,
            active_recuperation_mapping_enabled=False,
        )
    if not args.run:
        print(json.dumps(plan, indent=2))
        return 0
    if not all((args.interface, args.gateway, args.stationary, args.conditions, args.output)):
        parser.error("Live check requires --interface --gateway --stationary --conditions --output")
    try:
        address = ipaddress.IPv4Address(args.gateway)
        if not address.is_link_local or address in (
            ipaddress.IPv4Address("169.254.0.0"),
            ipaddress.IPv4Address("169.254.255.255"),
        ):
            raise ValueError("Gateway must be an observed unicast link-local ENET peer")
    except ValueError as error:
        parser.error(str(error))
    # Create evidence before opening a socket; preserve partial results on failure.
    report = dict(
        plan,
        mode="live-one-shot-check",
        results=[],
        interface=args.interface,
        gateway=args.gateway,
        conditions=args.conditions,
        time_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    )
    with args.output.open("x", encoding="utf-8") as evidence:
        try:
            require_wired(args.interface)
            source, _ = interface_info(args.interface)
            if source == args.gateway:
                raise ValueError("Gateway equals local address")
            report["source"] = source
            with bound_socket(args.interface, source, socket.SOCK_STREAM) as s:
                s.settimeout(3)
                s.connect((args.gateway, 6801))
                report["results"] = collect(s, selected, required_identity)
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            report["error"] = str(error)
        evidence.write(json.dumps(report, indent=2) + "\n")
    failed = "error" in report or any(r["status"] in STOP for r in report["results"])
    print(
        json.dumps(
            {
                "evidence": str(args.output),
                "requests": len(report["results"]),
                "error": report.get("error"),
                "transport_failed": failed,
                "production_polling_enabled": False,
            },
            indent=2,
        )
    )
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
