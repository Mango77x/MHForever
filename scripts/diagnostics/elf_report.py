"""Summarize sections and static VFPU opcode families in a PSP homebrew ELF."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

FAMILIES = {0x18: "VFPU0", 0x19: "VFPU1", 0x1B: "VFPU3", 0x34: "VFPU4",
            0x37: "VFPU5", 0x3C: "VFPU6", 0x32: "lv.s", 0x36: "lv.q",
            0x3A: "sv.s", 0x3E: "sv.q", 0x35: "lv(l/r)", 0x3D: "sv(l/r)"}


def report(data):
    def span(offset, size):
        if offset + size > len(data):
            raise ValueError("ELF range extends past the end of the file")
        return memoryview(data)[offset:offset + size]

    header = struct.unpack("<16sHHIIIIIHHHHHH", span(0, 52))
    ident, _, machine, _, entry, _, shoff, _, _, _, _, stride, count, names_index = header
    if ident[:7] != b"\x7fELF\x01\x01\x01" or machine != 8:
        raise ValueError("expected a little-endian ELF32 MIPS file")
    if not count or stride < 40 or not 0 < names_index < count:
        raise ValueError("expected a regular ELF section table with names")
    span(shoff, stride * count)
    headers = [struct.unpack("<10I", span(shoff + i * stride, 40)) for i in range(count)]
    strings = bytes(span(*headers[names_index][4:6]))
    sections = []
    counts = Counter()
    for name_offset, kind, flags, address, offset, size, *_ in headers:
        end = strings.find(b"\0", name_offset)
        if end < 0:
            raise ValueError("invalid section name")
        name = strings[name_offset:end].decode("ascii", errors="replace")
        sections.append({"name": name, "address": address, "size": size})
        if kind == 1 and flags & 4:
            code = span(offset, size)
            for (word,) in struct.iter_unpack("<I", code[:size - size % 4]):
                family = FAMILIES.get(word >> 26)
                if family:
                    counts[family] += 1
    return {"sha256": hashlib.sha256(data).hexdigest(), "entry": entry,
            "sections": sections, "static_vfpu_families": dict(counts)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(report(args.elf.read_bytes()), indent=2))
    except (OSError, ValueError) as error:
        parser.error(str(error))
