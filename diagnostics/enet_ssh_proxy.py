#!/usr/bin/env python3
"""SSH byte transport bound to wired ENET; no route/firewall changes or auth.

Host authentication remains OpenSSH's pinned entrynav2-hu key. This helper
only connects the one physical HU address/SSH port, never scans or runs commands.
"""

import ipaddress
import os
import re
import select
import socket
import subprocess
import sys


def connect(interface):
    if sys.platform != "darwin" or not re.fullmatch(r"en[0-9]+", interface):
        raise ValueError("Explicit macOS ENET interface required")
    ports = subprocess.check_output(["/usr/sbin/networksetup", "-listallhardwareports"], text=True)
    matches = [b for b in ports.split("\n\n") if f"Device: {interface}\n" in b + "\n"]
    if len(matches) != 1 or any(v in matches[0] for v in ("Wi-Fi", "AirPort", "Bluetooth")):
        raise ValueError("Refusing a non-wired hardware port")
    info = subprocess.check_output(["/sbin/ifconfig", interface], text=True)
    address = re.search(r"\binet (\d+\.\d+\.\d+\.\d+) netmask (0x[0-9a-f]+)", info)
    if "status: active" not in info or not address or int(address[2], 16) != 0xFFFF0000:
        raise ValueError("ENET link/IPv4 /16 absent")
    if not ipaddress.IPv4Address(address[1]).is_link_local:
        raise ValueError("ENET source is not link-local")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        index = socket.if_nametoindex(interface)
        s.setsockopt(socket.IPPROTO_IP, 25, index)
        if s.getsockopt(socket.IPPROTO_IP, 25) != index:
            raise RuntimeError("IP_BOUND_IF verification failed")
        s.bind((address[1], 0))
        s.settimeout(8)
        s.connect(("169.254.1.0", 22))
        return s
    except BaseException:
        s.close()
        raise


def main():
    if len(sys.argv) != 2:
        raise ValueError("Usage: enet_ssh_proxy.py enN")
    with connect(sys.argv[1]) as s:
        while True:
            ready, _, _ = select.select([0, s], [], [], 30)
            if not ready:
                continue  # OpenSSH owns keepalive/liveness and authentication.
            if s in ready:
                data = s.recv(8192)
                if not data:
                    return
                while data:
                    data = data[os.write(1, data) :]
            if 0 in ready:
                data = os.read(0, 8192)
                if not data:
                    return
                s.sendall(data)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("ENET SSH transport:", error, file=sys.stderr)
        raise SystemExit(1) from error
