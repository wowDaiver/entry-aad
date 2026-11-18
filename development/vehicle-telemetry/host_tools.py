# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable host-only compiler, font and FreeType discovery."""

import shlex
import shutil
import subprocess
from pathlib import Path


def compiler():
    for name in ("/opt/homebrew/opt/llvm/bin/clang++", "clang++", "g++"):
        found = shutil.which(name)
        if found:
            return found
    raise RuntimeError("A C++11 host compiler is required")


def freetype_flags():
    if shutil.which("pkg-config"):
        result = subprocess.run(["pkg-config", "--cflags", "--libs", "freetype2"], capture_output=True, text=True)
        if result.returncode == 0:
            return [*shlex.split(result.stdout), "-lz"]
    if Path("/opt/X11/include/freetype2").is_dir():
        return ["-I/opt/X11/include/freetype2", "-L/opt/X11/lib", "-lfreetype", "-lz"]
    raise RuntimeError("FreeType development files and zlib are required")


def default_font():
    for name in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    ):
        if Path(name).is_file():
            return Path(name)
    raise RuntimeError("Provide --font with a local TrueType font; BMW fonts are not distributed")
