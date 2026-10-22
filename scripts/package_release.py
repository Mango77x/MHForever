#!/usr/bin/env python3
"""Assemble the Windows release zip from binaries built elsewhere.

  package_release.py --version 0.1.0 --launcher <MHForever.exe> --game <mhfu.exe> --fonts <fonts dir> [--out <dir>]

The zip contains our programs, the free fonts, the project and third-party licenses and a player README. It never contains
game data: the script refuses disc images, executables other than the two programs, and anything it does not know.
"""
import argparse
import hashlib
import pathlib
import re
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
LICENSES = {
    "LICENSE": ROOT / "LICENSE",
    "THIRD_PARTY.md": ROOT / "THIRD_PARTY.md",
    "licenses/at3_standalone-LICENSE.txt": ROOT / "third_party/at3_standalone/LICENSE",
    "licenses/psprecomp-LICENSE.txt": ROOT / "third_party/psprecomp/LICENSE",
    "licenses/ppsspp_prx-NOTICE.md": ROOT / "third_party/ppsspp_prx/NOTICE.md",
    "licenses/pokke_save_crypto-LICENSE.txt": ROOT / "third_party/pokke_save_crypto/LICENSE",
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True)
    ap.add_argument("--launcher", required=True, type=pathlib.Path)
    ap.add_argument("--game", required=True, type=pathlib.Path)
    ap.add_argument("--fonts", required=True, type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("."))
    args = ap.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[A-Za-z0-9.]+)?", args.version):
        print("error: --version must look like 0.1.0", file=sys.stderr)
        return 2

    files: dict[str, pathlib.Path] = {"MHForever.exe": args.launcher, "mhfu.exe": args.game}
    if not args.fonts.is_dir():
        print(f"error: {args.fonts} is not a folder", file=sys.stderr)
        return 2
    for font in sorted(args.fonts.iterdir()):
        if font.suffix.lower() == ".ttf" or font.name == "LICENSE":
            files[f"fonts/{font.name}"] = font
        else:
            print(f"error: unexpected file in the fonts folder: {font.name}", file=sys.stderr)
            return 2
    files.update(LICENSES)
    for name, path in files.items():
        if not path.is_file():
            print(f"error: missing {name}: {path}", file=sys.stderr)
            return 2
        if path.suffix.lower() in {".iso", ".cso", ".zso", ".bin", ".elf", ".prx", ".pbp"}:
            print(f"error: refusing to package {path.name}", file=sys.stderr)
            return 2

    readme = (ROOT / "dist/PLAYER_README.txt").read_text(encoding="utf-8").replace("@VERSION@", args.version)
    folder = f"MHForever-{args.version}"
    args.out.mkdir(parents=True, exist_ok=True)
    zip_path = args.out / f"{folder}-windows-x64.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr(f"{folder}/README.txt", readme.replace("\n", "\r\n"))
        for name, path in files.items():
            z.write(path, f"{folder}/{name}")
    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    (args.out / f"{zip_path.name}.sha256").write_text(f"{digest}  {zip_path.name}\n", encoding="utf-8")
    print(f"{zip_path} ({zip_path.stat().st_size / 1e6:.1f} MB)\nsha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
