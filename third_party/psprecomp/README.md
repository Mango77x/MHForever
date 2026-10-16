# PSPRecomp (vendored)

- Upstream: https://github.com/jessicanataliagta/PSPRecomp
- Commit: `f6e7d415c7f447b934cc3865a31eb725f353d659` (2026-08-13)
- License: MIT (see `LICENSE`)

Vendored parts: `src/`, `include/`, `tools/` (analyzer, recompiler, function dumper), `tests/`, `configs/`.
Not vendored: upstream's `profiles/` (game-specific, includes generated game code), `docs/`, Ghidra exporter.

Local changes:
- `CMakeLists.txt` rewritten to build as a subproject of MH-Forever (no profile support, no MSVC tuning options).
  The `dump_function` tool target is renamed `psp_dump_function`.

Plan (`docs/04-recompiler.md`): this code becomes the starting point of `platforms/psp/tools`. Once we start modifying it
substantially (overlay support, MH-Forever runtime ABI), it will move there with its MIT notice preserved.
