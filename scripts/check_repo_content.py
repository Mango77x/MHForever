#!/usr/bin/env python3
"""Guard against committing game data or derivatives (CONTRIBUTING.md).

Usage:
  check_repo_content.py              scan files tracked by git (or the working tree if not a git repo)
  check_repo_content.py --staged     scan files staged for commit (used by .githooks/pre-commit)
  check_repo_content.py --self-test  run the built-in tests

Exit code 0 when clean, 1 when a violation is found, 2 on usage errors.
"""

from __future__ import annotations

import fnmatch
import os
import subprocess
import sys
import tempfile
from pathlib import Path

MAX_FILE_BYTES = 2 * 1024 * 1024

# File extensions of the games of every console (images, dumps, packages, ROMs), by console, as `.gitignore` lists them:
# the self-test checks that the two agree. `.md` (Mega Drive), `.bin`, `.dat`, `.app`, `.elf` and archives are left out
# on purpose: documents and build outputs use them.
GAME_FILE_EXTENSIONS = (
    # Disc images of any console (PSP, PS1, PS2, GameCube, Wii, Xbox, Saturn, Dreamcast...)
    "iso cso zso ciso chd cue ccd mds mdf nrg img gdi cdi xiso",
    # Sony: PSP, PS1, PS2, PS3, PS4, Vita (executables and packages)
    "pbp ecm pkg rap edat self vpk",
    # Microsoft: Xbox, Xbox 360 (executables)
    "xex xbe xvd xvc",
    # Nintendo GameCube and Wii
    "gcm gcz rvz wia wbfs wad tgc dol",
    # Nintendo Wii U
    "wud wux wua rpx rpl",
    # Nintendo Switch
    "nsp xci nca nsz xcz nso nro pfs0",
    # Nintendo 3DS
    "3ds cci cxi cia cfa 3dsx",
    # Nintendo DS, GBA, Game Boy, NES, SNES, N64
    "nds dsi srl gba gb gbc sgb nes fds unf sfc smc swc fig z64 n64 v64 ndd",
    # Sega, NEC, SNK, WonderSwan, Atari and generic ROMs
    "smd sms gg 32x pce sgx ngp ngc ws wsc a26 a52 a78 lnx rom",
)

# Keys and BIOS dumps by name (they decrypt or run those games).
KEY_AND_BIOS_PATTERNS = [
    "*.keys",
    "*aes_keys.txt",
    "*boot9.bin",
    "*boot11.bin",
    "*otp.bin",
    "*seeddb.bin",
    "*scph*.bin",
]


def game_extensions() -> list[str]:
    return [ext for group in GAME_FILE_EXTENSIONS for ext in group.split()]


# Paths that must never be committed (matched against the repo-relative POSIX path).
FORBIDDEN_PATH_PATTERNS = [
    "user_data/*",
    "research/*",
    "*/generated/*",
    "generated/*",
    "*/overlays/*.bin",
    "*.prx.dec",
    "*eboot*.bin", "*eboot*.elf", "*boot.bin", "*data.bin", "*param.sfo",
    "*.at3", "*.pmf",
    "*.gpr", "*.rep/*",
    *[f"*.{ext}" for ext in game_extensions()],
    *KEY_AND_BIOS_PATTERNS,
]

# Exceptions to FORBIDDEN_PATH_PATTERNS.
ALLOWED_PATHS = {
    "research/README.md",
}

# Content signatures of game data, whatever the file is called. (offset, bytes, description)
FORBIDDEN_SIGNATURES = [
    # PSP and the other Sony consoles
    (0, b"~PSP", "encrypted PSP executable (~PSP header)"),
    (0, b"\x00PSF", "PARAM.SFO metadata"),
    (0x8001, b"CD001", "ISO9660 disc image"),
    (0, b"PSMF", "PSP movie (PSMF)"),
    (0, b"PS-X EXE", "PlayStation executable"),
    (0, b"SCE\x00", "PlayStation SELF executable"),
    (0, b"\x7fPKG", "PlayStation 3 package"),
    (0, b"\x7fCNT", "PlayStation 4 package"),
    # Nintendo 3DS
    (0x100, b"NCSD", "Nintendo 3DS cartridge image (CCI/3DS)"),
    (0x100, b"NCCH", "Nintendo 3DS title (CXI/CFA)"),
    (0, b"3DSX", "Nintendo 3DS executable (3DSX)"),
    # Nintendo Switch
    (0x100, b"HEAD", "Nintendo Switch cartridge image (XCI)"),
    (0, b"PFS0", "Nintendo Switch package (NSP)"),
    (0x200, b"NCA3", "Nintendo Switch content archive (NCA)"),
    (0x200, b"NCA2", "Nintendo Switch content archive (NCA)"),
    (0, b"NSO0", "Nintendo Switch executable (NSO)"),
    (0x10, b"NRO0", "Nintendo Switch executable (NRO)"),
    # GameCube, Wii, Wii U
    (0x1C, b"\xc2\x33\x9f\x3d", "GameCube disc"),
    (0x18, b"\x5d\x1c\x9e\xa3", "Wii disc"),
    (0, b"WBFS", "Wii disc (WBFS)"),
    (0, b"CISO", "compressed disc image (CISO)"),
    (0, b"RVZ\x01", "Dolphin disc image (RVZ)"),
    (0, b"WIA\x01", "Dolphin disc image (WIA)"),
    (0, b"\x01\xc0\x0b\xb1", "GameCube disc image (GCZ)"),
    (0, b"\x00\x00\x00\x20Is", "Wii channel (WAD)"),
    (0, b"WUX0", "Wii U disc image (WUX)"),
    # Xbox, Xbox 360
    (0, b"XEX2", "Xbox 360 executable (XEX)"),
    (0, b"XBEH", "Xbox executable (XBE)"),
    (0x10000, b"MICROSOFT*XBOX*MEDIA", "Xbox disc image"),
    # Handhelds and old consoles
    (4, b"\x24\xff\xae\x51\x69\x9a\xa2\x21", "Game Boy Advance ROM"),
    (0xC0, b"\x24\xff\xae\x51\x69\x9a\xa2\x21", "Nintendo DS ROM"),
    (0x104, b"\xce\xed\x66\x66\xcc\x0d\x00\x0b", "Game Boy ROM"),
    (0, b"NES\x1a", "NES ROM"),
    (0, b"\x80\x37\x12\x40", "Nintendo 64 ROM"),
    (0, b"\x37\x80\x40\x12", "Nintendo 64 ROM (byte-swapped)"),
    (0, b"\x40\x12\x37\x80", "Nintendo 64 ROM (little endian)"),
    (0x100, b"SEGA MEGA DRIVE", "Mega Drive ROM"),
    (0x100, b"SEGA GENESIS", "Genesis ROM"),
    (0, b"SEGA SEGASATURN", "Sega Saturn disc"),
    (0, b"SEGA SEGAKATANA", "Dreamcast disc"),
]

ELF_MAGIC = b"\x7fELF"
EM_MIPS = 8
EI_OSABI_WIIU = 0xCA  # Wii U executables (RPX/RPL) are ELF files with this OS ABI


def is_mips_elf(head: bytes) -> bool:
    if len(head) < 20 or not head.startswith(ELF_MAGIC):
        return False
    little_endian = head[5] == 1
    machine = int.from_bytes(head[18:20], "little" if little_endian else "big")
    return machine == EM_MIPS


def is_wiiu_elf(head: bytes) -> bool:
    return len(head) >= 8 and head.startswith(ELF_MAGIC) and head[7] == EI_OSABI_WIIU


def check_path(rel: str) -> str | None:
    if rel in ALLOWED_PATHS:
        return None
    lower = rel.lower()
    for pattern in FORBIDDEN_PATH_PATTERNS:
        if fnmatch.fnmatch(lower, pattern):
            return f"forbidden path pattern '{pattern}'"
    return None


def check_content(data: bytes) -> str | None:
    if len(data) > MAX_FILE_BYTES:
        return f"file larger than {MAX_FILE_BYTES} bytes"
    for offset, magic, description in FORBIDDEN_SIGNATURES:
        if data[offset:offset + len(magic)] == magic:
            return f"looks like {description}"
    if is_mips_elf(data[:64]):
        return "MIPS ELF binary (PSP executable or build output)"
    if is_wiiu_elf(data[:64]):
        return "Wii U executable (RPX/RPL)"
    return None


def run_git(args: list[str], cwd: Path) -> bytes:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True).stdout


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def list_tracked(root: Path) -> list[str]:
    try:
        out = run_git(["ls-files", "-z"], root)
        return [p for p in out.decode("utf-8").split("\0") if p]
    except (OSError, subprocess.CalledProcessError):
        skip = {".git", "build", "out", "user_data"}
        files = []
        for dirpath, dirnames, filenames in os.walk(root):
            rel_dir = Path(dirpath).relative_to(root)
            dirnames[:] = [d for d in dirnames if not (rel_dir == Path(".") and d in skip)]
            for name in filenames:
                files.append((rel_dir / name).as_posix())
        return files


def list_staged(root: Path) -> list[str]:
    out = run_git(["diff", "--cached", "--name-only", "-z", "--diff-filter=ACMR"], root)
    return [p for p in out.decode("utf-8").split("\0") if p]


def scan(root: Path, paths: list[str], staged: bool) -> list[str]:
    problems = []
    for rel in paths:
        reason = check_path(rel)
        if reason is None:
            try:
                data = run_git(["show", f":{rel}"], root) if staged else (root / rel).read_bytes()
            except (OSError, subprocess.CalledProcessError):
                continue
            reason = check_content(data)
        if reason:
            problems.append(f"{rel}: {reason}")
    return problems


def self_test() -> int:
    failures = []

    def expect(condition: bool, label: str) -> None:
        if not condition:
            failures.append(label)

    expect(check_path("user_data/exe/x.bin") is not None, "user_data blocked")
    expect(check_path("research/kb/notes.md") is not None, "research blocked")
    expect(check_path("research/README.md") is None, "research README allowed")
    expect(check_path("isos/game.ISO") is not None, "iso blocked (case-insensitive)")
    expect(check_path("x/ULES01213_EBOOT.BIN") is not None, "eboot blocked")
    expect(check_path("games/mhfu/generated/f.cpp") is not None, "generated code blocked")
    expect(check_path("docs/00-overview.md") is None, "docs allowed")
    expect(check_path("core/app/main.cpp") is None, "sources allowed")

    expect(check_content(b"~PSP" + b"\0" * 64) is not None, "~PSP content blocked")
    expect(check_content(b"\0PSF" + b"\0" * 64) is not None, "PSF content blocked")
    iso = bytearray(0x8010)
    iso[0x8001:0x8006] = b"CD001"
    expect(check_content(bytes(iso)) is not None, "ISO content blocked")
    mips_elf = bytearray(64)
    mips_elf[0:4] = ELF_MAGIC
    mips_elf[5] = 1
    mips_elf[18:20] = EM_MIPS.to_bytes(2, "little")
    expect(check_content(bytes(mips_elf)) is not None, "MIPS ELF blocked")
    x86_elf = bytearray(mips_elf)
    x86_elf[18:20] = (62).to_bytes(2, "little")
    expect(check_content(bytes(x86_elf)) is None, "x86-64 ELF allowed")
    expect(check_content(b"a" * (MAX_FILE_BYTES + 1)) is not None, "large file blocked")
    wiiu = bytearray(64)
    wiiu[0:4] = ELF_MAGIC
    wiiu[7] = EI_OSABI_WIIU
    expect(check_content(bytes(wiiu)) is not None, "Wii U ELF blocked")

    # Every console's extensions are blocked by name, in any case, and every signature by content.
    for ext in game_extensions():
        expect(check_path(f"x/Game.{ext.upper()}") is not None, f".{ext} blocked")
    for name in ("Monster Hunter 4 Ultimate (Europe).cci", "prod.keys", "dump/boot9.bin", "keys/aes_keys.txt"):
        expect(check_path(name) is not None, f"{name} blocked")
    for offset, magic, description in FORBIDDEN_SIGNATURES:
        data = bytearray(offset + len(magic) + 16)
        data[offset:offset + len(magic)] = magic
        expect(check_content(bytes(data)) is not None, f"{description} blocked by content")
    # What the repository really holds must not be caught: documents, sources and the Mega Drive's `.md` clash.
    for name in ("README.md", "docs/05-runtime-hle.md", "launcher/native/cli.cpp", "scripts/package_release.py", "data/table.bin",
                 "third_party/stb/stb_image.h", "dist/PLAYER_README.txt", "assets/branding/icon-256.png"):
        expect(check_path(name) is None, f"{name} allowed")
    # `.gitignore` lists the same extensions: the two lists cannot drift apart.
    ignore = (repo_root() / ".gitignore").read_text(encoding="utf-8")
    for ext in game_extensions():
        expect(f"*.{ext}\n" in ignore or f"*.{ext} " in ignore, f"*.{ext} is in .gitignore")
    expect(check_content(b"int main() { return 0; }\n") is None, "text allowed")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "ok.txt").write_text("hello\n", encoding="utf-8")
        (root / "bad.bin").write_bytes(b"~PSP" + b"\0" * 16)
        problems = scan(root, ["ok.txt", "bad.bin"], staged=False)
        expect(len(problems) == 1 and problems[0].startswith("bad.bin"), "scan finds exactly the bad file")

    for label in failures:
        print(f"SELF-TEST FAILED: {label}")
    print("self-test passed" if not failures else f"{len(failures)} self-test failure(s)")
    return 0 if not failures else 1


def main(argv: list[str]) -> int:
    if argv[1:] == ["--self-test"]:
        return self_test()
    if argv[1:] not in ([], ["--staged"]):
        print(__doc__)
        return 2
    staged = argv[1:] == ["--staged"]
    root = repo_root()
    paths = list_staged(root) if staged else list_tracked(root)
    problems = scan(root, paths, staged)
    if problems:
        print("Game data or derived content must never be committed (CONTRIBUTING.md):")
        for problem in problems:
            print(f"  {problem}")
        return 1
    print(f"content check passed ({len(paths)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
