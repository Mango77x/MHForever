#!/usr/bin/env python3
"""Runs the motiontest program with frame interpolation on and checks the pictures it showed.

  check_interpolation.py <hb_motiontest> <motiontest.elf> <memstick dir>

The program moves its shapes a fixed step per 30 fps frame. With interpolation there must be about two pictures per
frame, the shapes must move in equal steps from picture to picture (the halfway ones sit exactly between), and the
shape that never moves must not.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

FRAMES = 40
SCALE = 2  # the test window renders at twice the PSP resolution


def read_ppm(path):
    data = path.read_bytes()
    _, size, _, pixels = data.split(b"\n", 3)
    w, h = map(int, size.split())
    return w, h, pixels


def span(frame, color, psp_y, x_from=0, x_to=480):
    w, h, px = frame
    row = psp_y * SCALE * 544 // (272 * SCALE)
    xs = [x for x in range(x_from * SCALE, x_to * SCALE) if px[(row * w + x) * 3:(row * w + x) * 3 + 3] == color]
    return (min(xs), max(xs)) if xs else None


def main() -> int:
    runner, elf, memstick = sys.argv[1:4]
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, SDL_VIDEO_DRIVER="offscreen", MHF_GL_DUMP=tmp, MHF_GL_DUMP_EVERY="1", MHF_INTERPOLATE="1")
        result = subprocess.run([runner, elf, "--memstick", memstick, "--window"], env=env, capture_output=True,
                                text=True, timeout=120)
        if "motiontest: done" not in result.stdout + result.stderr:
            print("the program did not finish", result.stderr[-500:])
            return 1
        files = sorted(pathlib.Path(tmp).glob("gl_*.ppm"))[1:]  # the first picture is from before the game draws
        frames = [read_ppm(f) for f in files]

    failures = []
    if len(frames) < int(FRAMES * 1.8):
        failures.append(f"only {len(frames)} pictures for {FRAMES} frames")
    red = [span(f, b"\xff\x00\x00", 60) for f in frames]
    green = [span(f, b"\x00\xff\x00", 140) for f in frames]
    bar = [span(f, b"\xff\xff\xff", 256) for f in frames]
    if None in red[4:] or None in green[4:] or None in bar:
        failures.append("a shape is missing from a picture")
    else:
        steps = [b[0] - a[0] for a, b in zip(red[4:], red[5:])]
        exact = sum(1 for s in steps if s == 8 * SCALE // 2)  # 16 px per frame at 2x: 8 per picture
        if exact < 0.9 * len(steps):
            failures.append(f"red sprite steps are not even: {steps}")
        gsteps = [a[0] - b[0] for a, b in zip(green[4:], green[5:])]
        gexact = sum(1 for s in gsteps if s == 8)
        if gexact < 0.9 * len(gsteps):
            failures.append(f"green sprite steps are not even: {gsteps}")
        if len(set(bar)) != 1:
            failures.append(f"the static bar moved: {sorted(set(bar))}")
    if failures:
        print("\n".join(failures))
        return 1
    print(f"interpolation: {len(frames)} pictures, even steps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
