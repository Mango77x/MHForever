#!/usr/bin/env python3
"""Runs the motiontest program with frame interpolation on and checks the pictures it showed.

  check_interpolation.py <hb_motiontest> <motiontest.elf> <memstick dir> [fps ...]

The program moves its shapes a fixed step per 30 fps frame. At N times that rate there must be about N pictures per
frame, the shapes must move in equal steps from picture to picture (the in-between ones sit exactly on the line between
two flips), and the shape that never moves must not. Pictures that arrive late are dropped on purpose, so the checks
allow some of them to be missing on a slow machine.
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


def run(runner, elf, memstick, fps):
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, SDL_VIDEO_DRIVER="offscreen", MHF_GL_DUMP=tmp, MHF_GL_DUMP_EVERY="1", MHF_FPS=str(fps),
                   ASAN_OPTIONS="detect_leaks=0")
        result = subprocess.run([runner, elf, "--memstick", memstick, "--window"], env=env, capture_output=True,
                                text=True, timeout=180)
        if "motiontest: done" not in result.stdout + result.stderr:
            return None, result.stderr[-500:]
        files = sorted(pathlib.Path(tmp).glob("gl_*.ppm"))[1:]  # the first picture is from before the game draws
        return [read_ppm(f) for f in files], ""


def check(frames, fps):
    k = fps // 30
    failures = []
    if len(frames) < int(FRAMES * (1 + 0.6 * (k - 1))):
        failures.append(f"x{k}: only {len(frames)} pictures for {FRAMES} frames")
    red = [span(f, b"\xff\x00\x00", 60) for f in frames]
    green = [span(f, b"\x00\xff\x00", 140) for f in frames]
    bar = [span(f, b"\xff\xff\xff", 256) for f in frames]
    if None in red[4:] or None in green[4:] or None in bar:
        failures.append(f"x{k}: a shape is missing from a picture")
        return failures
    full = 16.0  # dump pixels a flip moves the red sprite (8 PSP pixels at 2x)
    steps = [b[0] - a[0] for a, b in zip(red[4:], red[5:])]
    # Every step is a whole share of a flip's move (a dropped picture makes it a multiple), never backwards or beyond.
    good = [s for s in steps if 0 < s <= full + 1 and abs(round(s / (full / k)) * (full / k) - s) <= 1.0]
    if len(good) < 0.9 * len(steps):
        failures.append(f"x{k}: red sprite steps are not even: {steps}")
    gsteps = [a[0] - b[0] for a, b in zip(green[4:], green[5:])]
    ggood = [s for s in gsteps if 0 < s <= full + 1 and abs(round(s / (full / k)) * (full / k) - s) <= 1.0]
    if len(ggood) < 0.9 * len(gsteps):
        failures.append(f"x{k}: green sprite steps are not even: {gsteps}")
    if len(set(bar)) != 1:
        failures.append(f"x{k}: the static bar moved: {sorted(set(bar))}")
    return failures


def main() -> int:
    runner, elf, memstick = sys.argv[1:4]
    rates = [int(a) for a in sys.argv[4:]] or [60]
    failures = []
    for fps in rates:
        frames, error = run(runner, elf, memstick, fps)
        if frames is None:
            failures.append(f"{fps} fps: the program did not finish {error}")
            continue
        found = check(frames, fps)
        failures += found
        if not found:
            print(f"{fps} fps: {len(frames)} pictures, even steps")
    if failures:
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
