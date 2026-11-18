#!/usr/bin/env python3
"""Offline host renderer checks. Does not start a VM or contact a vehicle."""

import argparse
import json
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from host_tools import compiler as host_compiler  # noqa: E402
from host_tools import default_font, freetype_flags  # noqa: E402

DEFAULT_FONT = default_font()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--font", type=Path, default=DEFAULT_FONT)
args = parser.parse_args()
compiler = host_compiler()
out = ROOT / "build"
out.mkdir(exist_ok=True)
exe, png = out / "dashboard-preview", out / "dashboard-preview.png"
subprocess.run(
    [
        str(compiler),
        "-std=c++11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-misleading-indentation",
        "-fsanitize=address,undefined",
        str(ROOT / "dashboard/renderer.cpp"),
        str(ROOT / "dashboard/host_preview.cpp"),
        str(ROOT / "telemetry_model.cpp"),
        *freetype_flags(),
        "-o",
        str(exe),
    ],
    check=True,
)
result = subprocess.run([str(exe), str(args.font), str(png)], check=True, capture_output=True, text=True)
data = png.read_bytes()
assert data[:8] == b"\x89PNG\r\n\x1a\n"
pos, compressed, dimensions = 8, bytearray(), None
while pos < len(data):
    size = struct.unpack_from(">I", data, pos)[0]
    kind, payload = data[pos + 4 : pos + 8], data[pos + 8 : pos + 8 + size]
    assert zlib.crc32(kind + payload) & 0xFFFFFFFF == struct.unpack_from(">I", data, pos + 8 + size)[0]
    if kind == b"IHDR":
        dimensions = struct.unpack(">IIBBBBB", payload)
    if kind == b"IDAT":
        compressed.extend(payload)
    pos += size + 12
assert pos == len(data) and dimensions == (1200, 424, 8, 2, 0, 0, 0)
assert len(zlib.decompress(compressed)) == (1200 * 3 + 1) * 424
report = dict(
    kind="host-renderer-check",
    sanitizer="address,undefined",
    dimensions=[1200, 424],
    png_crc_valid=True,
    png_bytes=len(data),
    vehicle_contacted=False,
    native_hmi_verified=False,
    output=result.stdout,
)
(out / "dashboard-renderer-check.json").write_text(json.dumps(report, indent=2) + "\n")
print(result.stdout, end="")
print("PNG: CRC, RGB dimensions and decompressed size passed")
