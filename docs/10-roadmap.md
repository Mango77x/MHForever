# 10 — Roadmap

Each milestone has verifiable acceptance criteria. Mark `[x]` when done with date/PR.

## Phase 0 — Foundations (no game needed)

- [x] Repository skeleton: `core/`, `platforms/psp/`, `games/mhfu/` (region metadata), `tests/`, `LICENSE` (GPL-2.0), `.gitignore`,
      `.gitattributes`, `THIRD_PARTY.md`, PSPRecomp vendored in `third_party/psprecomp/` (2026-10-16).
- [x] Root CMake with presets `linux-clang`, `windows-clang`, `windows-msvc`; builds and passes tests on Linux (2026-10-16).
      Windows build not yet verified locally.
- [x] GitHub Actions CI (content guard + build/test on Ubuntu and Windows) written; first run pending until the repo is on GitHub.
- [x] Content guard `scripts/check_repo_content.py` (paths, `~PSP`/PSF/ISO/PSMF signatures, MIPS ELF, size) + `.githooks/pre-commit`
      (enable with `git config core.hooksPath .githooks`) (2026-10-16).
- [x] pspdev toolchain in CI (Linux job) to build our own `homebrew/` (2026-10-16).
- [x] Evaluate PSPRecomp, ACX recomp, N64Recomp and mhp2g-decomp (2026-10-16; summary in `04-recompiler.md` §1).
**Acceptance**: CI green on both OSes building an empty executable and the homebrew.

## Phase 1 — Minimal PSP runtime (on our own homebrew)

- [x] `platforms/psp/tools`: ISO9660 reader, ELF loader (PSPRecomp), NID database `platforms/psp/hle/nids.csv` generated
      from PSPSDK by `scripts/extract_nids.py` (2026-10-16). PRX relocation still pending.
- [x] Recompiler `mhf_recomp` (function-level, `04` §5): ALU, memory, delay slots, likely branches, calls, tail calls, jump
      tables, non-local returns, FPU, VFPU (PSPRecomp semantics + additions). `cputest` 100/100 (2026-10-16).
- [x] Recompiler + runtime: overlays analyzed together with the executable, overlay-aware lookup, hooks by canonical
      name (`04` §4.4) (2026-10-16).
- [x] Runtime `mhf_psp_runtime`: guest memory, fibers + scheduler, threads, semaphores, event flags, lwmutex, SysMem,
      IoFileMgr/Stdio over host folders, sceDisplay/sceCtrl (headless), sceRtc, UtilsForUser, scePower, LoadExec.
      `hello` and `threadtest` 17/17 pass natively on Linux (2026-10-16).
- [ ] `core`: SDL3 window, Vulkan, SDL3 input, audio output.
- [x] GE software renderer covering through mode and 3D (`05` §6); `getest` checks pixels (2026-10-16).
- [x] Interpreter fallback for code the analysis did not reach (`04` §7), VFPU included; every homebrew test also runs
      fully interpreted (`MHF_INTERPRET_ONLY=1`) (2026-10-16).
**Acceptance**: hello/ctrltest run natively on Windows and Linux; cputest passes 100 %.

## Phase 2 — Freedom Unite to the main menu (ULES01213)

- [x] Dump hashes, `PARAM.SFO`, `BOOT.BIN` state (zeros) — 2026-10-16.
- [x] Decrypted executable available in `user_data/exe/` — 2026-10-16.
- [x] Import inventory: 272 functions / 34 libraries (`05` §1) — 2026-10-16.
- [ ] Local Ghidra project + MCP (`09`).
- [x] `DATA.BIN` container + overlay format understood; developer-only extractor `mhfu_extract_overlays` with synthetic tests;
      298 EU overlays extracted and validated (0 undecodable instructions); manifest versioned (2026-10-16).
- [x] Overlay loader located in the EU executable (request/finish/prepare, region table, active-slot array); hook point
      `mh_overlay_prepare` recorded in `regions/ULES01213/symbols.toml` (2026-10-16).
- [ ] Full VFPU in the recompiler + vfputest.
- [x] GE (software): T&L, vertex formats, swizzled/CLUT/DXT textures, framebuffers in VRAM (2026-10-16). Vulkan backend pending.
- [x] Developer build of the game: `games/mhfu/tools/prepare.py` + `-DMHF_MHFU_GENERATED=...` → `mhfu` executable that
      reads the ISO directly (2026-10-16).
- [x] `sceLibFont` over a free font (Liberation Sans + stb_truetype); menu and dialog text render (2026-10-16).
- [x] Reaches the main menu, character creation (name via the keyboard stub) and the first save prompt with scripted
      input (`--input`) (2026-10-16).
- [ ] Local golden boot trace.
**Acceptance**: boots, shows logos and reaches the main menu; input works; no fallback-interpreter use on that path.

## Phase 3 — Single-player playable

- [x] Audio: SAS + ATRAC3+ (music and effects) (2026-10-17). Remaining: SAS reverb, movie audio (sceMpeg).
- [x] Savedata (`sceUtility`) save/load/list/delete/sizes to plain folders, `homebrew/savetest` (2026-10-16).
- [x] Our own UI for the save list, message dialog and on-screen keyboard (`05` §5h, 2026-10-18): drawn over the game,
      gamepad and physical keyboard; verified in FU's character creation (name typed, autosave toast).
- [ ] GE: framebuffer manager (effects), Bézier if used.
- [ ] Overlays: stage and monster overlays dispatch correctly.
- [ ] Village, shop, Felyne kitchen, one gathering quest and one hunting quest completed.
- [ ] Performance measured on a modest PC and on Linux.
**Acceptance**: complete 3 quests and save/load without failures; stable framerate matching the original.

## Phase 4 — Launcher and packaging

- [x] Gamepad-first launcher (`07` §3b, 2026-10-17): React/Motion UI in the system web view, add copy with SHA-1
  verification, settings passed to the game, play time; friends/rooms UI waits for online play.
- [ ] Launcher: language selection, icon, signed updates.
- [ ] AppImage (Linux) and zip/installer (Windows). Deck testing by friends.
**Acceptance**: a user with no development tools goes from "I have my ISO" to "I'm playing" using only the launcher.

## Phase 5 — Multiplayer

- [ ] Harden HLE boundaries first (`08-multiplayer.md` §4.6: Windows drive-letter path escape in `host_path`, disable
  `host0:`, GE address bounds, pointer validation, fuzzing).
- [ ] `sceNetAdhoc*` HLE, virtual ad-hoc layer, adhoctest homebrew.
- [ ] Serverless P2P : local identity, friend codes, room codes, Mainline DHT, hole punching (libjuice),
  UPnP/PCP, IPv6, relay through group members, pairwise rendezvous for friends (`08-multiplayer.md` §4).
- [ ] (optional) aemu-compatible backend for PPSSPP interop.
**Acceptance**: 4 players (Windows + Linux/Deck, different networks) complete a quest together.

## Phase 6 — US and JP regions

- [ ] `symbols.toml`, `recomp.toml`, `overlays.toml` for ULUS10391 and ULJM05500; symbol-coverage test.
**Acceptance**: all three regions meet phase 3 criteria.

## Phase 7 — Optional enhancements

Internal resolution, widescreen, analog camera, per-controller icons, 60 fps (research). See `06-mhfu-module.md` §9.

## Phase 8 — Next game

Portable 3rd or Freedom 2/Freedom (PSP), reusing the runtime. Then PS2 (Monster Hunter 1/G/2 "Dos") once PS2Recomp is usable, and
3DS (MH4U, Generations) once a base exists.

## Backlog / ideas

- Import PSP/PPSSPP saves (careful: save encryption).
- Local "download server" for event quests the player already owns (`05` §1, P3).
- Recording, voice chat.
- macOS/ARM64 ports.
- Clean, progressive reimplementation of modules (camera, UI, network) to depend less on translated code.
