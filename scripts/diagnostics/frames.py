"""Compare matching captures by RGB pixels or make a labeled contact sheet."""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageOps


def frames(folder):
    if not folder.is_dir():
        raise ValueError(f"missing directory: {folder}")
    paths = sorted(p for p in folder.iterdir() if p.suffix.lower() in {".ppm", ".png", ".bmp"} and p.is_file())
    if not paths:
        raise ValueError(f"no frames in {folder}")
    return {p.name: p for p in paths}


def compare(left, right):
    a, b = frames(left), frames(right)
    failures = 0
    for name in sorted(a.keys() | b.keys()):
        if name not in a or name not in b:
            print(f"{name}: missing from {left if name not in a else right}")
            failures += 1
            continue
        with Image.open(a[name]) as first, Image.open(b[name]) as second:
            if first.size != second.size:
                print(f"{name}: dimensions {first.size} vs {second.size}")
            else:
                bounds = ImageChops.difference(first.convert("RGB"), second.convert("RGB")).getbbox()
                if bounds is None:
                    continue
                print(f"{name}: changed pixels within {bounds}")
            failures += 1
    print(f"{len(a.keys() | b.keys())} filenames, {failures} differences")
    return int(bool(failures))


def sheet(folder, output, step, columns):
    if step < 1 or columns < 1:
        raise ValueError("step and columns must be positive")
    paths = list(frames(folder).values())[::step]
    cols = min(columns, len(paths))
    rows = (len(paths) + cols - 1) // cols
    canvas = Image.new("RGB", (240 * cols, 156 * rows), "white")
    draw = ImageDraw.Draw(canvas)
    for i, path in enumerate(paths):
        x, y = i % cols * 240, i // cols * 156
        with Image.open(path) as picture:
            canvas.paste(ImageOps.contain(picture.convert("RGB"), (240, 136)), (x, y))
        draw.text((x, y + 136), path.name, fill="black")
    canvas.save(output, "PNG")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    diff = commands.add_parser("compare")
    diff.add_argument("left", type=Path)
    diff.add_argument("right", type=Path)
    montage = commands.add_parser("sheet")
    montage.add_argument("folder", type=Path)
    montage.add_argument("output", type=Path)
    montage.add_argument("--step", type=int, default=1)
    montage.add_argument("--columns", type=int, default=6)
    args = parser.parse_args()
    try:
        if args.action == "compare":
            return compare(args.left, args.right)
        sheet(args.folder, args.output, args.step, args.columns)
    except (OSError, ValueError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    sys.exit(main())
