#!/usr/bin/env python3
"""Generates Freedom's recompiled code from the developer's own disc image (developer-only; nothing it writes may be
committed, CONTRIBUTING.md).

usage: games/mhf1/tools/prepare.py --iso <image.iso> --exe <prepared executable> --tools <build dir> [--region ULES00318]

The overlays are extracted with the build's mhfu_extract_overlays (Freedom stores them in DATA.BIN as Freedom Unite
does, with the same cipher), then mhf_recomp writes the code to user_data/generated/mhf1/<region>/. Configure with
-DMHF_GAMES=mhf1 -DMHF_MHF1_GENERATED=<that folder>.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


def tool(build_dir, name):
    for sub in ("games/mhfu", "platforms/psp", "."):
        for ext in (".exe", ""):
            path = os.path.join(build_dir, sub, name + ext)
            if os.path.isfile(path):
                return path
    sys.exit(f"error: {name} not found in {build_dir} (build the tools first)")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--iso", required=True)
    p.add_argument("--exe", required=True)
    p.add_argument("--tools", required=True, help="CMake build folder that contains mhf_recomp and mhfu_extract_overlays")
    p.add_argument("--region", default="ULES00318")
    p.add_argument("--parts", default="16")
    a = p.parse_args()

    user_data = os.path.join(ROOT, "user_data")
    overlays = os.path.join(user_data, "overlays", "mhf1", a.region)
    generated = os.path.join(user_data, "generated", "mhf1", a.region)
    region_dir = os.path.join(ROOT, "games", "mhf1", "regions", a.region)

    subprocess.run([tool(a.tools, "mhfu_extract_overlays"), "--iso", a.iso, "--exe", a.exe, "--out", overlays], check=True)
    subprocess.run([tool(a.tools, "mhf_recomp"), "--elf", a.exe, "--out", generated, "--name", "mhf1", "--parts", a.parts,
                    "--overlays", os.path.join(overlays, "overlays.toml"),
                    "--symbols", os.path.join(region_dir, "symbols.toml"),
                    "--hook", "mh_overlay_prepare", "--hook", "mh_draw_wait"], check=True)
    print(f"\nGenerated code: {generated}\nConfigure with: -DMHF_GAMES=mhf1 -DMHF_MHF1_GENERATED={generated}")


if __name__ == "__main__":
    main()
