# Third-party components

Every dependency included in `third_party/` is listed here with its license, version/commit and origin.
Read `docs/02-architecture.md` §9 before adding anything.

| Component | Path | License | Version | Origin |
|---|---|---|---|---|
| PSPRecomp framework | `third_party/psprecomp/` | MIT | `f6e7d41` (2026-08-13) | https://github.com/jessicanataliagta/PSPRecomp |
| PSPRecomp instruction emitter (derived) | `platforms/psp/recomp/instruction_emitter.cpp`, branch conditions in `emitter.cpp` | MIT | `f6e7d41` | moved out of PSPRecomp's `tools/codegen_main.cpp`, notice kept in the file |
| stb_truetype | `third_party/stb/` | MIT or public domain | v1.26 | https://github.com/nothings/stb |
| Liberation Sans Regular (font) | `third_party/fonts/liberation-sans/` | SIL OFL 1.1 | 2.1.5 | https://github.com/liberationfonts/liberation-fonts (TTF as packaged by Debian `fonts-liberation` 1:2.1.5) |

Fetched at build time (not stored in the repository):

| Component | Used by | License | Version | Origin |
|---|---|---|---|---|
| SDL3 | `core/host` (window, input, audio) | zlib | 3.2.24 (SHA-256 pinned in `cmake/sdl3.cmake`) | https://github.com/libsdl-org/SDL |
| pspdev toolchain + PSPSDK | builds our own `homebrew/` tests only | GPL/BSD (tools), BSD (SDK) | latest release | https://github.com/pspdev/pspdev |

Generated data: `platforms/psp/hle/nids.csv` (library, NID, function name) is extracted by `scripts/extract_nids.py` from the
PSPSDK stub libraries (pspdev, BSD-licensed). It contains identifiers only, no SDK code.

Knowledge sources (no code copied): `tclamb/mhp2g-decomp` (CC0) documented the DATA.BIN header layout, the scrambling scheme and
the `MWo3` overlay header used by `games/mhfu/tools/`. The game's substitution table itself is never included.

Never include: Sony PGF fonts, firmware, keys, decryption code, game assets.
