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

# Paths that must never be committed (matched against the repo-relative POSIX path).
FORBIDDEN_PATH_PATTERNS = [
    "user_data/*",
    "research/*",
    "*/generated/*",
    "generated/*",
    "*/overlays/*.bin",
    "*.iso", "*.cso", "*.chd", "*.pbp", "*.prx.dec",
    "*eboot*.bin", "*eboot*.elf", "*boot.bin", "*data.bin", "*param.sfo",
    "*.at3", "*.pmf",
    "*.gpr", "*.rep/*",
]

# Exceptions to FORBIDDEN_PATH_PATTERNS.
ALLOWED_PATHS = {
    "research/README.md",
}

# Content signatures of PSP game data. (offset, bytes, description)
FORBIDDEN_SIGNATURES = [
    (0, b"~PSP", "encrypted PSP executable (~PSP header)"),
    (0, b"\x00PSF", "PARAM.SFO metadata"),
    (0x8001, b"CD001", "ISO9660 disc image"),
    (0, b"PSMF", "PSP movie (PSMF)"),
]

ELF_MAGIC = b"\x7fELF"
EM_MIPS = 8


def is_mips_elf(head: bytes) -> bool:
    if len(head) < 20 or not head.startswith(ELF_MAGIC):
        return False
    little_endian = head[5] == 1
    machine = int.from_bytes(head[18:20], "little" if little_endian else "big")
    return machine == EM_MIPS


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
