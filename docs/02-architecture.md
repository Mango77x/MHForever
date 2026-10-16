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
                   │  + platforms/psp/gpu (GE→Vulkan), audio           │
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

1. **core/** — console-independent: window and input (SDL3), audio output, Vulkan base, config, logging, network
   (P2P/relay), debug overlay (Dear ImGui), launcher.
2. **platforms/psp/** — knows the PSP but no specific game:
   - `tools/`: ISO9660 reader, ELF/PRX loader, function analyzer, Allegrex→C++ recompiler, overlay support.
   - `runtime/`: memory, CPU context, threads, HLE of system libraries (by NID), overlay manager, fallback interpreter.
   - `gpu/`: GE→Vulkan.
   - `audio/`: SAS, ATRAC3+, PCM.
3. **games/mhfu/** — the profile: regions (hashes, symbol maps), recompiler configuration, overlay table, patches, native fast
   paths, optional enhancements, game tests.

Dependency rule: `games → platforms → core`. Never the other way. `core` and `platforms` contain no game addresses.

## 2. Repository layout

```
/
├─ CONTRIBUTING.md           contribution rules
├─ README.md
├─ LICENSE                   (GPL-2.0-only)
├─ THIRD_PARTY.md
├─ CMakeLists.txt            root; option -DMHF_GAMES="mhfu"
├─ CMakePresets.json
├─ cmake/                    toolchains, helpers
├─ core/
│  ├─ app/  input/  audio/  render/  net/  config/  log/  ui/  launcher/
├─ platforms/psp/
│  ├─ tools/   (isoread, elfload, analyze, recomp, overlays)
│  ├─ runtime/ (memory, context, kernel, io, ctrl, display, power, rtc, utility, net, overlay manager, ...)
│  ├─ gpu/     (GE parser, state, Vulkan backend, texture cache, framebuffer manager)
│  ├─ audio/   (sas, atrac, mpeg/psmf)
│  └─ data/    (NID database: system function names, no Sony code)
├─ games/mhfu/
│  ├─ game.toml               module metadata
│  ├─ regions/ULES01213/      region.toml, symbols.toml, recomp.toml, overlays.toml, patches/
│  ├─ regions/ULUS10391/
│  ├─ regions/ULJM05500/
│  ├─ host/                   game-specific HLE, fast paths, integration
│  ├─ enhancements/           widescreen, fps, analog camera (toggleable)
│  └─ tests/
├─ homebrew/                  our own PSP programs (pspdev) used by tests
├─ tests/                     core + platform tests
├─ third_party/
├─ docs/
├─ research/                  git-ignored except README.md
└─ user_data/                 ALWAYS git-ignored: isos/, exe/, overlays/, generated/, build-game/, saves/
```

## 3. Player pipeline

1. **Import**: the player picks their ISO.
2. **Identify**: hash (SHA-1) of the ISO → known region in `games/*/regions/*/region.toml`. Unknown hash: clear message, stop.
3. **Index**: the ISO9660 reader indexes the files (and an LBA manifest, since PSP games sometimes read by sector).
4. **Play**: the launcher starts the release binary pointing at the ISO. The runtime serves file reads from it.

Open point: the executable's initial `.data`/`.rodata` contents are needed at run time. How they reach the runtime is
still being decided; this repository never contains them.

## 4. Developer pipeline (release preparation, local only)

1. Prepare the input executable from your own copy (outside the repo) → `user_data/exe/`.
2. Extract overlays from `DATA.BIN` using the game's own loading logic knowledge → `user_data/overlays/` (`04-recompiler.md` §4).
3. Analyze + recompile with the region's `recomp.toml`, `symbols.toml`, `overlays.toml` → `user_data/generated/`.
4. Build with symbols and debug overlay for development, or as a release binary.

Alternatively (model B), steps 1–3 move to the player's machine (requires a bundled toolchain, `07-launcher-ux.md` §4).

## 5. Technology choices

| Area | Choice | Rationale / alternative |
|---|---|---|
| Language | C++20 (runtime/tools); generated output in C++ | Same stack as N64Recomp/PSPRecomp |
| Build | CMake ≥ 3.20 + Ninja, presets | Standard; PSPRecomp and N64Recomp use it |
| Compiler | Clang on both OSes (clang-cl or MSVC on Windows acceptable) | Uniform behavior |
| Window/input/audio | SDL3 | Controllers, Steam Input, Deck |
| Rendering | Vulkan 1.2+ | Deck/Linux/Windows (DX12 optional later) |
| Debug UI | Dear ImGui (MIT) | ACX recomp uses the same |
| Config | TOML (toml++) | N64Recomp uses TOML |
| Network | libjuice (MPL-2.0, ICE) or GameNetworkingSockets (BSD-3) | See `08-multiplayer.md` |
| MIPS decoding | PSPRecomp decoder (MIT): covers all but 55 words of FU's main `.text` | Measured, `04-recompiler.md` §1 |
| Tests | doctest or Catch2 + ctest | |
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

## 9. Licenses (GPL-2.0-only)

| Candidate component | License | Compatible with MIT repo | Compatible with GPL-2.0 repo |
|---|---|---|---|
| PSPRecomp | MIT | yes | yes |
| N64Recomp | MIT | yes | yes |
| Ace Combat X Recompilation | GPL-2.0 (no "or later" in sources → GPL-2.0-only `[VERIFY]`) | no (if code is copied) | yes, GPL-2.0-only |
| PPSSPP | GPL-2.0-or-later | no | yes |
| SDL3 | zlib | yes | yes |
| Dear ImGui | MIT | yes | yes |
| libjuice | MPL-2.0 | yes (file-level) | yes |
| mhff / mhtools (MH formats) | GPL-3.0 | no | no (GPL-3 only) — reference only |
| pspdev/vfpu-docs | GPL-3.0 (docs and tests) | reference only | reference only |

**Decision**: the repository is **GPL-2.0-only**. Code from ACX recomp, PPSSPP (GPL-2.0-or-later, usable under GPL-2.0) and MIT/zlib/MPL projects can be reused with its notices; GPL-3-only code (mhff, mhtools, vfpu-docs) stays reference-only.

Original rationale: if code from ACX recomp or PPSSPP is to be **copied** (e.g. the ATRAC3+ decoder ACX takes from
FFmpeg/PPSSPP), pick **GPL-2.0-only**. If MIT is preferred, all code must be ours or permissive, and PPSSPP/ACX can only be used
as behavioral reference (PSPRecomp's rule: "Reimplement required behavior from specifications, observations or independently
documented semantics"). Decide before writing the first line of runtime.

For ATRAC3/ATRAC3+, FFmpeg's decoders (LGPL-2.1+) are an option compatible with almost any license if linked dynamically or if
the LGPL terms are respected. **Done (2026-10-17)**: `third_party/at3_standalone` (FFmpeg's decoders as extracted by
PPSSPP), used under GPL-2.0 as LGPL-2.1-or-later permits; see `THIRD_PARTY.md`.
