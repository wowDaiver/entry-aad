#!/usr/bin/env python3
"""One bounded read-only EGS source check. No sessions, scans, writes or retries.

Fixed address 0x18, service 0x22. Identification F150/F197, then only the
published temperature records 6000/DA12. Unknown variants remain raw evidence.
"""

import datetime
import json
import socket
import struct
from pathlib import Path

import read_identity
from enet import bound_socket, interface_info

NAMES = {
    0xF150: "sgbd_identification",
    0xF197: "system_name",
    0x6000: "transmission_oil_temperature_native",
    0xDA12: "transmission_oil_temperature_bus",
}


def request_frame(target, did):
    if target != 0x18 or did not in NAMES:
        raise ValueError("Only fixed EGS temperature/identity reads permitted")
    return struct.pack("!IHBBBH", 5, 1, 0xF4, target, 0x22, did)


def main():
    # Explicitly observed ENET peer; do not discover or search other addresses here.
    source, _ = interface_info("en8")
    report = {
        "time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "gateway": "169.254.76.216",
        "target": "0x18",
        "service": "0x22 only",
        "conditions": "stationary, engine off, head unit on (user confirmed)",
        "results": [],
    }
    # Reuse the bounded HSFZ receiver, with this module's narrower fixed target/list.
    read_identity.request_frame = request_frame
    read_identity.DIDS = NAMES
    with bound_socket("en8", source, socket.SOCK_STREAM) as s:
        s.settimeout(3)
        s.connect((report["gateway"], 6801))
        for did in (0xF150, 0xF197):
            rec = read_identity.read_one(s, 0x18, did, 3)
            report["results"].append(rec)
            if rec["status"] in ("incomplete", "transport_rejected", "frame_limit"):
                break
        if len(report["results"]) == 2 and report["results"][0]["status"] == "positive":
            for did in (0x6000, 0xDA12):
                rec = read_identity.read_one(s, 0x18, did, 3)
                report["results"].append(rec)
                if rec["status"] in ("incomplete", "transport_rejected", "frame_limit"):
                    break
    out = Path(__file__).resolve().parents[1] / "research/evidence/egs-source-2026-11-16/egs-check-engine-off.json"
    out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
