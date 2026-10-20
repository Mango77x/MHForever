# 02 — Architecture

## 1. Overview

```
┌──────────────────────────────── Launcher (launcher/) ───────────────────────────────┐
│ Library · Import copy · Validate hash · Index data · Settings · Multiplayer lobby    │
└───────────────────────────────────────┬──────────────────────────────────────────────┘
                                        │ launches
                                        ▼
                   ┌──────────────────────────────────────────────────┐
                   │ Game executable (release binary, no assets)       │
                   │  recompiled code (main EBOOT + overlays)          │
                   │  + platforms/psp/runtime (HLE)                    │
                   │  + platforms/psp/gpu (GE→OpenGL), audio           │
                   │  + games/mhfu (profile, patches, enhancements)    │
                   │  + core (input, audio out, net, UI)               │
                   └───────────────────────┬──────────────────────────┘
                                           │ reads all data at runtime
                                           ▼
                                 player's ISO (DATA.BIN, etc.)

Developer machine only (never CI):
  decrypted EBOOT + overlays extracted from own ISO
     → platforms/psp/tools (analyzer → recompiler) → user_data/generated/*.cpp → release binary
```

Layers, bottom-up:

1. **core/** — console-independent: the SDL3 host (window, input, audio output) and base utilities. Config, logging,
   network (P2P/relay) and a debug overlay are planned.
2. **platforms/psp/** — knows the PSP but no specific game:
   - `tools/`: ISO9660 and PARAM.SFO readers.
   - `recomp/`: function analyzer and Allegrex→C++ emitter (`mhf_recomp`); the ELF loader and MIPS decoder come from
     `third_party/psprecomp`.
   - `runtime/`: memory, CPU context, kernel (fibers, threads, semaphores...), HLE of system libraries (by NID), SAS and
     ATRAC3+ audio, overlay manager, fallback interpreter.
   - `gpu/`: GE list processing, vertex decoding, T&L and the software renderer, behind a backend interface.
   - `frontend/`: glue to the SDL3 host, the OpenGL 3.3 backend and the system dialogs drawn over the game.
   - `hle/`: the NID database (system function names, no Sony code).
3. **games/mhfu/** — the profile: regions (hashes, symbol maps), recompiler configuration, overlay table, patches, native fast
   paths, optional enhancements, game tests.

Dependency rule: `games → platforms → core`. Never the other way. `core` and `platforms` contain no game addresses.

## 2. Repository layout

```
/
├─ CONTRIBUTING.md  README.md  BUILDING.md  THIRD_PARTY.md  LICENSE (GPL-3.0-or-later)
├─ CMakeLists.txt            root; option -DMHF_GAMES="mhfu"
├─ CMakePresets.json
├─ cmake/                    SDL3 and webview helpers
├─ core/                     app/ (entry point), base/ (version), host/ (SDL3 window, input, audio)
├─ platforms/psp/
│  ├─ tools/                 ISO9660 and PARAM.SFO readers
│  ├─ recomp/                analyzer and Allegrex→C++ emitter (mhf_recomp)
│  ├─ runtime/               src/ (kernel, HLE libraries, interpreter, fibers), include/mhf/psp/, host/
│  ├─ gpu/                   GE processing, software renderer, backend interface
│  ├─ frontend/              SDL3 glue, OpenGL 3.3 backend, system dialogs
│  ├─ hle/                   NID database (system function names, no Sony code)
│  └─ tests/
├─ games/mhfu/
│  ├─ game.toml              module metadata
│  ├─ regions/ULES01213/     region.toml, symbols.toml, overlays.toml (ULUS10391 and ULJM05500: region.toml for now)
│  ├─ host/                  game-specific host (command line, overlay hook, integration)
│  ├─ tools/                 DATA.BIN reader, overlay extractor, prepare.py
│  └─ tests/
├─ launcher/                 native/ (+ install/), ui/ (React), tests/
├─ homebrew/                 our own PSP programs (pspdev) used by tests
├─ tests/                    repository-level tests; support/check.hpp
├─ scripts/                  content guard, NID extraction, release packaging
├─ dist/                     text shipped with the release
├─ third_party/              psprecomp, at3_standalone, ppsspp_prx, stb, fonts
├─ docs/
├─ research/                 git-ignored except README.md
└─ user_data/                ALWAYS git-ignored: isos/, exe/, overlays/, generated/, build-game/, saves/
```

Planned and not present yet: `games/mhfu/enhancements/` and per-region `patches/`, a network module in `core/`, and the
US and JP symbol maps.

## 3. Player pipeline

1. **Import**: the player picks their ISO.
2. **Identify**: hash (SHA-1) of the ISO → known region in `games/*/regions/*/region.toml`. Unknown hash: clear message, stop.
3. **Prepare**: the launcher's installer reads the game's executable from the image, checks it against the region's
   identification values and prepares it in the per-user data folder (`07-launcher-ux.md` §3b). The image can optionally be
   copied there too.
4. **Play**: the launcher starts the release binary pointing at the ISO and the prepared executable. The runtime serves file
   reads from the ISO (including raw-sector reads, since PSP games sometimes read by sector).

The executable's initial `.data`/`.rodata` contents come from that prepared executable; this repository never contains it.

## 4. Developer pipeline (release preparation, local only)

1. Prepare the input executable from your own copy (outside the repo) → `user_data/exe/`.
2. Extract overlays from `DATA.BIN` using the game's own loading logic knowledge → `user_data/overlays/` (`04-recompiler.md` §4).
3. Analyze + recompile with the region's `symbols.toml` and the extracted `overlays.toml` → `user_data/generated/`
   (`games/mhfu/tools/prepare.py` runs steps 2 and 3).
4. Build with symbols and debug overlay for development, or as a release binary.

Alternatively (model B), steps 1–3 move to the player's machine (requires a bundled toolchain, `07-launcher-ux.md` §4).

## 5. Technology choices

| Area | Choice | Rationale / alternative |
|---|---|---|
| Language | C++20 (runtime/tools); generated output in C++ | Same stack as N64Recomp/PSPRecomp |
| Build | CMake ≥ 3.20 + Ninja, presets | Standard; PSPRecomp and N64Recomp use it |
| Compiler | Clang on both OSes (clang-cl or MSVC on Windows acceptable) | Uniform behavior |
| Window/input/audio | SDL3 | Controllers, Steam Input, Deck |
| Rendering | OpenGL 3.3 (implemented); Vulkan 1.2+ possible later | Same code on Windows, Linux and Deck; backend interface keeps Vulkan open |
| Debug UI | Dear ImGui (MIT), planned | Not used yet; ACX recomp uses the same |
| Config | TOML, read by a small in-house parser (`mini_toml.hpp`) | N64Recomp uses TOML |
| Network | libjuice (MPL-2.0, ICE) or GameNetworkingSockets (BSD-3), planned | Phase 5, see `08-multiplayer.md` |
| MIPS decoding | PSPRecomp decoder (MIT): covers all but 55 words of FU's main `.text` | Measured, `04-recompiler.md` §1 |
| Tests | In-house `tests/support/check.hpp` + ctest | Dependency-free |
| CI | GitHub Actions: Windows + Ubuntu, no game data | |

## 6. Module configuration (draft format)

`games/mhfu/game.toml`
```toml
id = "mhfu"
platform = "psp"
display_name = "Freedom Unite (PSP)"   # descriptive only, never the product name
regions = ["ULES01213", "ULUS10391", "ULJM05500"]
```

`games/mhfu/regions/ULES01213/region.toml`
```toml
disc_id = "ULES01213"
disc_version = "1.01"
market = "EU"
languages = ["en", "fr", "de", "es", "it"]   # [VERIFY]

[[accepted_dumps]]
kind = "iso"
size = 886702080
sha1 = "631e3d1651550617ed4b83351d874e6c06bd56b0"

[dev_executable]                             # developer-only input, never shipped
sha1 = "04b60ee797bd628faa0525ebd52186fa05967963"
```

`symbols.toml`: canonical names → addresses in this region (`06-mhfu-module.md` §5).

## 7. Multi-region

- Every patch, hook or fast path targets a **canonical name** (`mh_camera_update`), never an address.
- Each region has its own `symbols.toml`. When a function is found in EU, its counterpart in US/JP is located (byte signatures,
  referenced strings, Ghidra Version Tracking/BinDiff) and added to the map.
- If a symbol is missing in a region, patches using it are disabled there and a warning is logged.

## 8. Performance

A literal instruction-by-instruction translation compiled with -O2 should be more than enough on any modern PC and on the Deck
for a 222/333 MHz game, but **measure, don't assume**. Typical wins: inlining direct calls, `switch` for jump tables, native fast
paths for hot functions (memcpy, VFPU math).

## 9. Licenses (GPL-3.0-or-later)

| Candidate component | License | Compatible with MIT repo | Compatible with GPL-3.0+ repo |
|---|---|---|---|
| PSPRecomp | MIT | yes | yes |
| N64Recomp | MIT | yes | yes |
| Ace Combat X Recompilation | GPL-2.0 (no "or later" in sources → GPL-2.0-only `[VERIFY]`) | no (if code is copied) | yes (GPL-2.0-only code cannot be combined: reference only unless it says "or later") |
| PPSSPP | GPL-2.0-or-later (libkirk: GPL-3.0-or-later) | no | yes |
| SDL3 | zlib | yes | yes |
| Dear ImGui | MIT | yes | yes |
| libjuice | MPL-2.0 | yes (file-level) | yes |
| mhff / mhtools (MH formats) | GPL-3.0 | no | yes |
| pspdev/vfpu-docs | GPL-3.0 (docs and tests) | reference only | reference only |

**Decision (2026-10-19)**: the repository is **GPL-3.0-or-later**, changed from GPL-2.0-only before anything was published (single copyright holder, no external contributions). Reason: PPSSPP's PRX decrypter needs libkirk's `kirk_engine`, which is GPL-3.0-or-later, and the installer needs it to decrypt the player's own executable. Code from PPSSPP (GPL-2.0-or-later, used under GPL-3), GPL-3 projects (mhff, mhtools, vfpu-docs) and MIT/zlib/MPL/LGPL/Apache projects can be reused with their notices. **GPL-2.0-only** code (the ACX recomp, if its sources carry no "or later") cannot be copied; it stays a behavioral reference.

For ATRAC3/ATRAC3+, FFmpeg's decoders (LGPL-2.1+) are an option compatible with almost any license if linked dynamically or if
the LGPL terms are respected. **Done (2026-10-17)**: `third_party/at3_standalone` (FFmpeg's decoders as extracted by
PPSSPP), used under GPL-3.0-or-later as LGPL-2.1-or-later permits; see `THIRD_PARTY.md`.
