"""Bounded, read-only ELF32 little-endian inspection. No guest code is executed.

Only the ELF32 subset needed by the archived HU files is accepted. ARM pointer
relocations are resolved only for linked ARM executables/DSOs. Addresses
returned by this module are ELF virtual addresses, never live process addresses.
"""

import struct
from pathlib import Path


class ELFError(ValueError):
    pass


class ELF32:
    def __init__(self, source):
        self.data = Path(source).read_bytes() if isinstance(source, (str, Path)) else bytes(source)
        self._span(0, 52)
        if self.data[:7] != b"\x7fELF\x01\x01\x01":
            raise ELFError("expected ELF32 little-endian version 1")
        h = self._unpack("HHIIIIIHHHHHH", 16)
        (
            self.etype,
            self.machine,
            _,
            self.entry,
            phoff,
            shoff,
            self.flags,
            ehsize,
            phsize,
            phnum,
            shsize,
            shnum,
            shstrndx,
        ) = h
        if ehsize != 52:
            raise ELFError("invalid ELF32 header size")
        if (phnum and phsize != 32) or (shnum and shsize != 40):
            raise ELFError("unsupported table entry size")
        if not shnum or shstrndx >= shnum:
            raise ELFError("missing/extended section table unsupported")
        self._span(phoff, phsize * phnum)
        self._span(shoff, shsize * shnum)
        self.segments = [self._unpack("8I", phoff + i * phsize) for i in range(phnum)]
        for t, off, va, _, filesz, memsz, _, _ in self.segments:
            if t == 1 and (filesz > memsz or va + memsz > 2**32):
                raise ELFError("invalid load segment")
            self._span(off, filesz)
        raw = [self._unpack("10I", shoff + i * shsize) for i in range(shnum)]
        for s in raw:
            if s[1] != 8:  # SHT_NOBITS has no bytes in the file.
                self._span(s[4], s[5])
        shstr = self._slice(raw[shstrndx][4], raw[shstrndx][5])
        self.sections = []
        for s in raw:
            self.sections.append(
                dict(
                    zip(
                        ("name_offset", "type", "flags", "va", "offset", "size", "link", "info", "align", "entsize"), s
                    ),
                    name=self._cstring(shstr, s[0]),
                )
            )
        self.symbol_tables = {}
        self.symbols = {}
        for i, s in enumerate(self.sections):
            if s["type"] not in (2, 11):
                continue
            if s["entsize"] != 16 or s["size"] % 16 or s["link"] >= len(raw):
                raise ELFError("invalid symbol table")
            strings = self.section_data(s["link"])
            table = []
            for off in range(s["offset"], s["offset"] + s["size"], 16):
                name, va, size, info, other, section = self._unpack("IIIBBH", off)
                entry = dict(
                    name=self._cstring(strings, name),
                    va=va,
                    size=size,
                    type=info & 15,
                    binding=info >> 4,
                    other=other,
                    section=section,
                )
                table.append(entry)
                if entry["name"] and section:
                    self.symbols[entry["name"]] = entry
            self.symbol_tables[i] = table
        self.relocations = {}
        for s in self.sections:
            if s["type"] != 9:  # SHT_REL (these ARM binaries do not use RELA).
                continue
            if self.etype == 1 or self.machine != 40:
                # ET_REL offsets refer to target sections, not linked VAs.
                # They can overlap between sections; never index them as VAs.
                continue
            if s["entsize"] != 8 or s["size"] % 8 or s["link"] not in self.symbol_tables:
                raise ELFError("invalid relocation table")
            table = self.symbol_tables[s["link"]]
            for off in range(s["offset"], s["offset"] + s["size"], 8):
                va, info = self._unpack("II", off)
                index = info >> 8
                if index >= len(table):
                    raise ELFError("invalid relocation symbol")
                if va in self.relocations:
                    raise ELFError("overlapping relocations unsupported")
                self.relocations[va] = dict(type=info & 255, symbol=table[index])

    def _span(self, offset, size):
        if offset < 0 or size < 0 or offset > len(self.data) or size > len(self.data) - offset:
            raise ELFError("short read/out of file bounds")

    def _slice(self, offset, size):
        self._span(offset, size)
        return self.data[offset : offset + size]

    def _unpack(self, fmt, offset):
        return struct.unpack("<" + fmt, self._slice(offset, struct.calcsize("<" + fmt)))

    @staticmethod
    def _cstring(data, offset):
        if not 0 <= offset < len(data):
            raise ELFError("string offset outside table")
        end = data.find(b"\0", offset)
        if end < 0:
            raise ELFError("unterminated string")
        return data[offset:end].decode("utf-8", "replace")

    def section_data(self, index):
        s = self.sections[index]
        if s["type"] == 8:
            raise ELFError("NOBITS section is not file-backed")
        return self._slice(s["offset"], s["size"])

    def file_offset(self, va, size=1):
        if va < 0 or size < 0 or va + size > 2**32:
            raise ELFError("invalid virtual address span")
        matches = [
            off + va - start
            for t, off, start, _, length, _, _, _ in self.segments
            if t == 1 and start <= va and va + size <= start + length
        ]
        if len(matches) != 1:
            raise ELFError("VA is not uniquely file-backed (BSS/unmapped/overlap)")
        self._span(matches[0], size)
        return matches[0]

    def word(self, va):
        return self._unpack("I", self.file_offset(va, 4))[0]

    def pointer(self, va):
        if self.machine != 40 or self.etype not in (2, 3):
            raise ELFError("pointer resolution requires linked ARM ELF")
        raw = self.word(va)
        rel = self.relocations.get(va)
        if not rel:
            return dict(kind="literal", raw=hex(raw), elf_va=hex(raw))
        sym = rel["symbol"]
        out = dict(kind="relocation", raw=hex(raw), relocation_type=rel["type"], symbol=sym["name"])
        if rel["type"] == 2 and sym["section"]:  # R_ARM_ABS32: S + A, base still implicit for ET_DYN.
            out["elf_va"] = hex((sym["va"] + raw) & 0xFFFFFFFF)
        elif rel["type"] == 23:  # R_ARM_RELATIVE: B + A. B is unavailable offline.
            out["elf_va"] = hex(raw)
            out["runtime_requires_load_bias"] = True
        else:
            out["unresolved"] = True
        return out

    def metadata(self):
        needed, soname, paths, buildids = [], None, {}, []
        for i, s in enumerate(self.sections):
            if s["type"] == 6:
                if s["entsize"] != 8 or s["size"] % 8 or s["link"] >= len(self.sections):
                    raise ELFError("invalid dynamic table")
                strings = self.section_data(s["link"])
                for off in range(s["offset"], s["offset"] + s["size"], 8):
                    tag, value = self._unpack("II", off)
                    if tag == 1:
                        needed.append(self._cstring(strings, value))
                    elif tag == 14:
                        soname = self._cstring(strings, value)
                    elif tag in (15, 29):
                        paths[{15: "rpath", 29: "runpath"}[tag]] = self._cstring(strings, value)
            if s["type"] == 7:
                data = self.section_data(i)
                cursor = 0
                while cursor < len(data):
                    if len(data) - cursor < 12:
                        raise ELFError("truncated note")
                    namesz, descsz, kind = struct.unpack_from("<III", data, cursor)
                    cursor += 12
                    nend = cursor + namesz
                    dend = ((nend + 3) & ~3) + descsz
                    if dend > len(data):
                        raise ELFError("note out of bounds")
                    name = data[cursor:nend].rstrip(b"\0")
                    if name == b"GNU" and kind == 3:
                        buildids.append(data[(nend + 3) & ~3 : dend].hex())
                    cursor = (dend + 3) & ~3
        if len(set(buildids)) > 1:
            raise ELFError("conflicting build IDs")
        interpreter = None
        for t, off, _, _, size, _, _, _ in self.segments:
            if t == 3:
                interpreter = self._slice(off, size).rstrip(b"\0").decode()
        return dict(
            elf_class=32,
            machine={40: "ARM", 140: "TMS320C6000"}.get(self.machine, str(self.machine)),
            machine_number=self.machine,
            endian="little",
            elf_type={1: "ET_REL", 2: "ET_EXEC", 3: "ET_DYN"}.get(self.etype, self.etype),
            flags=hex(self.flags),
            build_id=next(iter(buildids), None),
            needed=needed,
            soname=soname,
            interpreter=interpreter,
            **paths,
        )
