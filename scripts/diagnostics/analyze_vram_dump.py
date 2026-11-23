"""Offline analysis of probe_vram_dump.py output (read-only).

usage: analyze_vram_dump.py DUMP_DIR PHASE SRC_NAME VRAM_OFFSET
Checks that VRAM holds the DMA source at VRAM_OFFSET, then classifies every
CALL into VRAM found in the dumped list region by where it lands.
"""
import struct
import argparse
import sys
from collections import Counter
from pathlib import Path

RET, CALL, BASE, PRIM, END = 0x0B, 0x0A, 0x10, 0x04, 0x0C


def words(data):
    return struct.unpack(f"<{len(data) // 4}I", data[: len(data) // 4 * 4])


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("phase")
    parser.add_argument("source")
    parser.add_argument("offset", type=lambda s: int(s, 16), help="hexadecimal byte offset")
    args = parser.parse_args(argv)
    folder, phase, src_name, offset = args.folder, args.phase, args.source, args.offset
    vram = (folder / f"{phase}-vram.bin").read_bytes()
    src = (folder / f"{phase}-{src_name}.bin").read_bytes()
    lists = (folder / f"{phase}-lists.bin").read_bytes()
    if not src or offset < 0 or offset + len(src) > len(vram):
        parser.error("source must fit entirely inside the VRAM capture")
    if any(value % 4 for value in (offset, len(src), len(lists), len(vram))):
        parser.error("expected word-aligned offsets and captures")
    window = vram[offset: offset + len(src)]
    same = sum(a == b for a, b in zip(window, src))
    print(f"{phase}: VRAM[{offset:#x}..+{len(src):#x}] equals source on {same}/{len(src)} bytes")
    first_diff = next((i for i, (a, b) in enumerate(zip(window, src)) if a != b), None)
    if first_diff is not None:
        print(f"  first difference at VRAM {0x04000000 + offset + first_diff:#010x}")
    vw = words(vram)
    # Sub-list starts: the region start and every word after a RET.
    starts = {0x04000000 + offset} | {0x04000000 + (i + 1) * 4 for i, w in enumerate(vw)
                                      if w >> 24 == RET and offset <= i * 4 < offset + len(src)}
    lw = words(lists)
    targets = Counter()
    for i, w in enumerate(lw):
        if w >> 24 != CALL:
            continue
        base = next((x for x in reversed(lw[max(0, i - 6): i]) if x >> 24 == BASE), None)
        if base is None:
            continue
        target = (((base >> 16) & 0xF) << 24 | (w & 0xFFFFFF)) & 0x0FFFFFFC
        if 0x04000000 <= target < 0x04200000:
            targets[target] += 1
    inside = [t for t in targets if 0x04000000 + offset <= t < 0x04000000 + offset + len(src)]
    good = [t for t in inside if t in starts]
    print(f"  CALL sites into VRAM: {sum(targets.values())}, distinct targets {len(targets)}, "
          f"inside copied lists {len(inside)}, on a sub-list start {len(good)}")
    bad = sorted(set(inside) - set(good))[:8]
    for t in bad:
        i = (t - 0x04000000) // 4
        print(f"  off-start {t:#010x}: " + " ".join(f"{x:08X}" for x in vw[max(0, i - 2): i + 4]))
    outside = sorted(set(targets) - set(inside))[:8]
    if outside:
        print("  outside copied lists:", " ".join(f"{t:#010x}" for t in outside))
    prims = Counter((w >> 16) & 7 for w in vw[offset // 4:(offset + len(src)) // 4] if w >> 24 == PRIM)
    print(f"  PRIM commands in copied lists by type: {dict(prims)}")


if __name__ == "__main__":
    main(sys.argv[1:])
