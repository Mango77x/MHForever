# Third-party components

Every dependency included in `third_party/` is listed here with its license, version/commit and origin.
Read `docs/02-architecture.md` §9 before adding anything.

| Component | Path | License | Version | Origin |
|---|---|---|---|---|
| PSPRecomp framework | `third_party/psprecomp/` | MIT | `f6e7d41` (2026-08-13) | https://github.com/jessicanataliagta/PSPRecomp |
| PSPRecomp instruction emitter (derived) | `platforms/psp/recomp/instruction_emitter.cpp`, branch conditions in `emitter.cpp` | MIT | `f6e7d41` | moved out of PSPRecomp's `tools/codegen_main.cpp`, notice kept in the file |
| stb_truetype | `third_party/stb/` | MIT or public domain | v1.26 | https://github.com/nothings/stb |
| ATRAC3/ATRAC3+ decoders (FFmpeg, standalone extraction) | `third_party/at3_standalone/` | LGPL-2.1-or-later (used under GPL-2.0) | PPSSPP `ext/at3_standalone` at `0665ea0` (2026-10) | https://github.com/hrydgard/ppsspp (code from https://ffmpeg.org). Our changes: `arch_config.h`, `compat.cpp` and the allocator in `mem.cpp` replace PPSSPP's helpers; `aac_defines.h` dropped |
| Liberation Sans Regular (font) | `third_party/fonts/liberation-sans/` | SIL OFL 1.1 | 2.1.5 | https://github.com/liberationfonts/liberation-fonts (TTF as packaged by Debian `fonts-liberation` 1:2.1.5) |

Fetched at build time (not stored in the repository):

| Component | Used by | License | Version | Origin |
|---|---|---|---|---|
| webview | `launcher/` (system web view host) | MIT | 0.12.0 (`3ab4b5d`, `cmake/webview.cmake`) | https://github.com/webview/webview |
| WebView2 SDK headers | `launcher/` on Windows (headers only; the runtime is part of Windows) | BSD-3-Clause | 1.0.1150.38 | NuGet `Microsoft.Web.WebView2` |
| React, React DOM | launcher UI, bundled into the launcher executable | MIT | 19.3.1 (`launcher/ui/package-lock.json`) | https://react.dev |
| Motion | launcher UI animations, bundled | MIT | 14.3.0 | https://motion.dev |
| Sora, Manrope (fonts via Fontsource) | launcher UI, bundled | SIL OFL 1.1 | 5.3.0 | https://fontsource.org |
| Vite, vite-plugin-singlefile, TypeScript | build tools for the launcher UI (not shipped) | MIT / Apache-2.0 | see `package.json` | npm |
| SDL3 | `core/host` (window, input, audio) | zlib | 3.2.24 (SHA-256 pinned in `cmake/sdl3.cmake`) | https://github.com/libsdl-org/SDL |
| pspdev toolchain + PSPSDK | builds our own `homebrew/` tests only | GPL/BSD (tools), BSD (SDK) | latest release | https://github.com/pspdev/pspdev |

Generated data: `platforms/psp/hle/nids.csv` (library, NID, function name) is extracted by `scripts/extract_nids.py` from the
PSPSDK stub libraries (pspdev, BSD-licensed). It contains identifiers only, no SDK code.

Knowledge sources (no code copied): `tclamb/mhp2g-decomp` (CC0) documented the DATA.BIN header layout, the scrambling scheme and
the `MWo3` overlay header used by `games/mhfu/tools/`. The game's substitution table itself is never included.

Never include: Sony PGF fonts, firmware, keys, decryption code, game assets.
