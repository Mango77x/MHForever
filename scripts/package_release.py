#!/usr/bin/env python3
"""Assemble a release archive from binaries built elsewhere.

  package_release.py --launcher <MHForever[.exe]> --game <mhfu[.exe]> --fonts <fonts dir>
                     [--platform windows|linux] [--version <version>] [--out <dir>]

The version defaults to the one in `project(MHForever VERSION ...)` (CMakeLists.txt), so archives, the launcher and the
core program never drift apart; pass --version only to override it.

The archive contains our programs, the free fonts, the project and third-party licenses and a player README. It never
contains game data: the script refuses disc images, executables other than the two programs, and anything it does not know.

Windows produces MHForever-<version>-windows-x64.zip; Linux produces MHForever-<version>-linux-x64.tar.gz.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import pathlib
import re
import sys
import tarfile
import time
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
LICENSES = {
    "LICENSE": ROOT / "LICENSE",
    "THIRD_PARTY.md": ROOT / "THIRD_PARTY.md",
    "licenses/psprecomp-LICENSE.txt": ROOT / "third_party/psprecomp/LICENSE",
    "licenses/ppsspp_prx-NOTICE.md": ROOT / "third_party/ppsspp_prx/NOTICE.md",
    "licenses/pokke_save_crypto-LICENSE.txt": ROOT / "third_party/pokke_save_crypto/LICENSE",
    # FFmpeg's own licence and source note: the library is never shipped in the archive, only the notice that says
    # where it comes from (the distribution's package on Linux, the pinned build on Windows).
    "licenses/FFmpeg-SOURCE.txt": ROOT / "third_party/ffmpeg/FFmpeg-SOURCE.txt",
    "licenses/FFmpeg-LICENSE.txt": ROOT / "third_party/ffmpeg/FFmpeg-LICENSE.txt",
}

# Platform -> (launcher name, game name, player README, archive suffix).
PLATFORMS = {
    "windows": ("MHForever.exe", "mhfu.exe", "dist/PLAYER_README.txt", "windows-x64"),
    "linux": ("MHForever", "mhfu", "dist/PLAYER_README_LINUX.txt", "linux-x64"),
}


def project_version() -> str:
    """The single source of truth for the version: `project(MHForever VERSION x.y.z)` in CMakeLists.txt."""
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"^project\(\s*MHForever\s+VERSION\s+([0-9][0-9.]*)\b", text, re.MULTILINE)
    if match is None:
        raise SystemExit("error: cannot read project(MHForever VERSION ...) from CMakeLists.txt")
    return match.group(1)


def check_inputs(args, launcher_name: str, game_name: str) -> dict[str, pathlib.Path] | None:
    files: dict[str, pathlib.Path] = {launcher_name: args.launcher, game_name: args.game}
    if not args.fonts.is_dir():
        print(f"error: {args.fonts} is not a folder", file=sys.stderr)
        return None
    for font in sorted(args.fonts.iterdir()):
        if font.suffix.lower() == ".ttf" or font.name == "LICENSE":
            files[f"fonts/{font.name}"] = font
        else:
            print(f"error: unexpected file in the fonts folder: {font.name}", file=sys.stderr)
            return None
    files.update(LICENSES)
    for name, path in files.items():
        if not path.is_file():
            print(f"error: missing {name}: {path}", file=sys.stderr)
            return None
        if path.suffix.lower() in {".iso", ".cso", ".zso", ".bin", ".elf", ".prx", ".pbp"}:
            print(f"error: refusing to package {path.name}", file=sys.stderr)
            return None
    return files


def write_zip(path: pathlib.Path, folder: str, readme: str, files: dict[str, pathlib.Path]) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr(f"{folder}/README.txt", readme.replace("\n", "\r\n"))
        for name, source in files.items():
            z.write(source, f"{folder}/{name}")


def write_tarball(path: pathlib.Path, folder: str, readme: str, files: dict[str, pathlib.Path],
                  executables: set[str]) -> None:
    with tarfile.open(path, "w:gz", compresslevel=9) as t:
        readme_bytes = readme.encode("utf-8")
        info = tarfile.TarInfo(f"{folder}/README.txt")
        info.size = len(readme_bytes)
        info.mode = 0o644
        info.mtime = int(time.time())
        t.addfile(info, io.BytesIO(readme_bytes))
        for name, source in files.items():
            info = t.gettarinfo(str(source), arcname=f"{folder}/{name}")
            if info is None:  # only symlinks can fail here, and the inputs are regular files
                continue
            info.mode = 0o755 if name in executables else 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            with source.open("rb") as handle:
                t.addfile(info, handle)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", default=None,
                    help="release version (default: the version in CMakeLists.txt)")
    ap.add_argument("--launcher", required=True, type=pathlib.Path)
    ap.add_argument("--game", required=True, type=pathlib.Path)
    ap.add_argument("--fonts", required=True, type=pathlib.Path)
    ap.add_argument("--platform", choices=sorted(PLATFORMS), default="windows",
                    help="target platform of the programs (default: windows)")
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("."))
    args = ap.parse_args()
    if args.version is None:
        args.version = project_version()
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[A-Za-z0-9.]+)?", args.version):
        print("error: --version must look like 0.1.0", file=sys.stderr)
        return 2

    launcher_name, game_name, readme_name, suffix = PLATFORMS[args.platform]
    files = check_inputs(args, launcher_name, game_name)
    if files is None:
        return 2

    readme = (ROOT / readme_name).read_text(encoding="utf-8").replace("@VERSION@", args.version)
    folder = f"MHForever-{args.version}"
    args.out.mkdir(parents=True, exist_ok=True)
    if args.platform == "windows":
        path = args.out / f"{folder}-{suffix}.zip"
        write_zip(path, folder, readme, files)
    else:
        path = args.out / f"{folder}-{suffix}.tar.gz"
        write_tarball(path, folder, readme, files, {launcher_name, game_name})
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    (args.out / f"{path.name}.sha256").write_text(f"{digest}  {path.name}\n", encoding="utf-8")
    print(f"{path} ({path.stat().st_size / 1e6:.1f} MB)\nsha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
