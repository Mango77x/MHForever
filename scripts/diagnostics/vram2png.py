"""Decode the visible RGB channels of one linear PSP framebuffer."""
import argparse
from pathlib import Path

from PIL import Image


def decode(raw, fmt, width, height, stride):
    bpp = 4 if fmt == "8888" else 2
    if width < 1 or height < 1 or stride < width:
        raise ValueError("expected positive dimensions and stride >= width")
    if len(raw) != stride * height * bpp:
        raise ValueError("incomplete framebuffer")
    layouts = {"565": (5, 6, 5), "5551": (5, 5, 5), "4444": (4, 4, 4)}
    pixels = bytearray()
    for row in range(height):
        start = row * stride * bpp
        for i in range(start, start + width * bpp, bpp):
            if fmt == "8888":
                pixels.extend(raw[i:i + 3])
                continue
            word = int.from_bytes(raw[i:i + 2], "little")
            for bits in layouts[fmt]:
                mask = (1 << bits) - 1
                pixels.append((word & mask) * 255 // mask)
                word >>= bits
    return Image.frombytes("RGB", (width, height), bytes(pixels))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("dump", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("format", choices=("565", "5551", "4444", "8888"))
    p.add_argument("--width", type=int, default=480)
    p.add_argument("--height", type=int, default=272)
    p.add_argument("--stride", type=int, default=512, help="pixels per stored row")
    p.add_argument("--offset", type=lambda s: int(s, 0), default=0, help="byte offset")
    args = p.parse_args()
    if min(args.offset, args.width - 1, args.height - 1, args.stride - args.width) < 0:
        p.error("invalid offset, dimensions or stride")
    size = args.stride * args.height * (4 if args.format == "8888" else 2)
    try:
        with args.dump.open("rb") as source:
            source.seek(args.offset)
            if size > args.dump.stat().st_size - args.offset:
                p.error("incomplete framebuffer")
            picture = decode(source.read(size), args.format, args.width, args.height, args.stride)
        picture.save(args.output, "PNG")
    except OSError as error:
        p.error(str(error))


if __name__ == "__main__":
    main()
