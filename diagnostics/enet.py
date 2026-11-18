#!/usr/bin/env python3
"""Bounded ENET discovery. No routing changes, sessions, writes or subnet scans.

BMW bytes: ediabaslib EdInterfaceEnet.UdpIdentReq.
DoIP: ISO 13400 vehicle identification, payload type 0x0001.
Darwin IP_BOUND_IF=25: installed SDK netinet/in.h.
"""

import argparse
import datetime
import ipaddress
import json
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

HSFZ_IDENT = bytes.fromhex("000000000011")
DOIP_IDENT = bytes.fromhex("02fd000100000000")


def interface_info(name):
    if not re.fullmatch(r"en\d+", name):
        raise ValueError("Only explicitly named Ethernet interfaces enN are supported")
    p = subprocess.run(["/sbin/ifconfig", name], capture_output=True, text=True, check=True)
    m = re.search(r"\binet (\d+\.\d+\.\d+\.\d+) netmask (0x[0-9a-f]+)", p.stdout)
    if "status: active" not in p.stdout or not m:
        raise ValueError(f"{name} must have active physical link and IPv4")
    addr, mask = m.groups()
    network = ipaddress.IPv4Network((addr, str(ipaddress.IPv4Address(int(mask, 16)))), strict=False)
    if network != ipaddress.IPv4Network("169.254.0.0/16"):
        raise ValueError("This tool requires the expected ENET link-local /16; refusing another network")
    return addr, str(network.broadcast_address)


def bound_socket(interface, source, kind):
    if sys.platform != "darwin":
        raise RuntimeError("Live probes supported only on macOS; parsers work offline anywhere")
    s = socket.socket(socket.AF_INET, kind)
    try:
        index = socket.if_nametoindex(interface)
        s.setsockopt(socket.IPPROTO_IP, 25, index)
        if s.getsockopt(socket.IPPROTO_IP, 25) != index:
            raise RuntimeError("Interface binding verification failed")
        s.bind((source, 0))
        return s
    except BaseException:
        s.close()
        raise


def parse_hsfz(data):
    if len(data) < 6:
        raise ValueError("Short HSFZ header")
    size, control = struct.unpack("!IH", data[:6])
    if control != 0x11 or size != len(data) - 6 or not data[6:].startswith(b"DIAGADR10"):
        raise ValueError("Not a valid gateway identification response")
    result = {"protocol": "hsfz", "gateway_diagnostic_address": "0x10"}
    for marker, count, name in [(b"BMWMAC", 12, "mac"), (b"BMWVIN", 17, "vin")]:
        pos = data.find(marker, 15)
        if pos < 0 or pos + len(marker) + count > len(data):
            raise ValueError("Incomplete gateway identification")
        value = data[pos + len(marker) : pos + len(marker) + count].decode("ascii")
        if name == "mac" and not re.fullmatch(r"[0-9A-Fa-f]{12}", value):
            raise ValueError("Invalid gateway MAC")
        if name == "vin" and not re.fullmatch(r"[A-HJ-NPR-Z0-9]{17}", value):
            raise ValueError("Invalid gateway VIN")
        result[name] = value
    return result


def parse_doip(data):
    if len(data) < 8:
        raise ValueError("Short DoIP header")
    version, inverse, kind, length = struct.unpack("!BBHI", data[:8])
    if version not in (2, 3) or version ^ inverse != 255 or kind != 4:
        raise ValueError("Not a DoIP identification response")
    if length not in (32, 33) or len(data) != length + 8:
        raise ValueError("Invalid DoIP identification length")
    vin, address, eid, gid, action = struct.unpack("!17sH6s6sB", data[8:40])
    return {
        "protocol": "doip",
        "vin": vin.decode("ascii"),
        "logical_address": f"0x{address:04x}",
        "eid": eid.hex(),
        "gid": gid.hex(),
        "further_action": action,
        "sync_status": data[40] if length == 33 else None,
    }


def discover(interface, source, broadcast, timeout):
    results = []
    for port, request, parser in [(6811, HSFZ_IDENT, parse_hsfz), (13400, DOIP_IDENT, parse_doip)]:
        record = {"port": port, "request_hex": request.hex(), "destination": broadcast, "responses": []}
        with bound_socket(interface, source, socket.SOCK_DGRAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            s.sendto(request, (broadcast, port))
            record["local_socket"] = s.getsockname()
            end = time.monotonic() + timeout
            while time.monotonic() < end and len(record["responses"]) < 16:
                s.settimeout(max(0.01, end - time.monotonic()))
                try:
                    data, peer = s.recvfrom(4096)
                except socket.timeout:
                    break
                response = {"peer": peer, "hex": data.hex()}
                try:
                    if peer[1] != port or not ipaddress.IPv4Address(peer[0]).is_link_local or peer[0] == source:
                        raise ValueError("Unexpected response endpoint")
                    response["identification"] = parser(data)
                except (ValueError, UnicodeError) as e:
                    response["parse_error"] = str(e)
                record["responses"].append(response)
        results.append(record)
    return results


def tcp_probe(interface, source, target, ports, timeout):
    if not ipaddress.IPv4Address(target).is_link_local or target == source:
        raise ValueError("Target must be a peer link-local address")
    if not set(ports) <= {22, 6801, 13400} or len(ports) > 3:
        raise ValueError("Only TCP/22, /6801, /13400 allowed, once per port")
    results = []
    for port in dict.fromkeys(ports):
        record = {"ip": target, "port": port}
        try:
            with bound_socket(interface, source, socket.SOCK_STREAM) as s:
                s.settimeout(timeout)
                s.connect((target, port))
                record["status"] = "connected"
                if port == 22:
                    # Receive only: no credentials, authentication or commands.
                    try:
                        record["banner"] = s.recv(512).decode("ascii", errors="replace")
                    except socket.timeout:
                        record["banner_status"] = "timeout"
        except OSError as e:
            record.update(status="unreachable_or_filtered", error=str(e), errno=e.errno)
        results.append(record)
    return results


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--interface", required=True)
    p.add_argument("--timeout", type=float, default=3)
    p.add_argument("--output", type=Path)
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("discover")
    tcp = sub.add_parser("tcp")
    tcp.add_argument("--target", required=True)
    tcp.add_argument("--ports", type=int, nargs="+", default=[22])
    a = p.parse_args()
    if not 0.1 <= a.timeout <= 10:
        p.error("timeout must be 0.1..10 seconds")
    report = {
        "time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "interface": a.interface,
        "command": a.command,
    }
    try:
        source, broadcast = interface_info(a.interface)
        report.update(source=source, broadcast=broadcast, binding="Darwin IP_BOUND_IF (25), verified with getsockopt")
        report["results"] = (
            discover(a.interface, source, broadcast, a.timeout)
            if a.command == "discover"
            else tcp_probe(a.interface, source, a.target, a.ports, a.timeout)
        )
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as e:
        report["error"] = str(e)
    encoded = json.dumps(report, indent=2)
    if a.output:
        a.output.write_text(encoded + "\n")
    print(encoded)
    return 1 if "error" in report else 0


if __name__ == "__main__":
    sys.exit(main())
