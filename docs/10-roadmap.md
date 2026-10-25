# 10 — Roadmap

Each milestone has verifiable acceptance criteria. Mark `[x]` when done with date/PR.

## Phase 0 — Foundations (no game needed)

- [x] Repository skeleton: `core/`, `platforms/psp/`, `games/mhfu/` (region metadata), `tests/`, `LICENSE` (GPL-3.0-or-later), `.gitignore`,
      `.gitattributes`, `THIRD_PARTY.md`, PSPRecomp vendored in `third_party/psprecomp/` (2026-10-16).
- [x] Root CMake with presets `linux-clang`, `windows-clang`, `windows-msvc`; builds and passes tests on Linux (2026-10-16)
      and on Windows (CI and the release build, 2026-10-19).
- [x] GitHub Actions CI (content guard + build/test on Ubuntu and Windows) green on `main` since 2026-10-19.
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
- [x] `core`: SDL3 window, input and audio output (`core/host/sdl_host.cpp`, 2026-10-16).
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
- [x] GE (software): T&L, vertex formats, swizzled/CLUT/DXT textures, framebuffers in VRAM (2026-10-16).
- [x] GE hardware renderer: OpenGL 3.3 behind the `gpu::Backend` interface, upscaling and render to texture
      (2026-10-17; known gaps in `05` §6).
- [ ] Vulkan backend (optional: OpenGL already covers Windows, Linux and the Deck).
- [x] Developer build of the game: `games/mhfu/tools/prepare.py` + `-DMHF_MHFU_GENERATED=...` → `mhfu` executable that
      reads the ISO directly (2026-10-16).
- [x] `sceLibFont` over a free font (Liberation Sans + stb_truetype); menu and dialog text render (2026-10-16).
- [x] Reaches the main menu, character creation (name via the keyboard stub) and the first save prompt with scripted
      input (`--input`) (2026-10-16).
- [ ] Local golden boot trace.
**Acceptance**: boots, shows logos and reaches the main menu; input works; no fallback-interpreter use on that path.

## Phase 3 — Single-player playable

- [x] Audio: SAS + ATRAC3+ (music and effects) (2026-10-17). Remaining: SAS reverb.
- [x] Pre-rendered movies (`sceMpeg`, `05` §5i, 2026-10-23): picture and sound, H.264 through FFmpeg's libavcodec and
      ATRAC3+ through `libavcodec` (cmake/ffmpeg.cmake). Verified with the intro; the gallery plays through the same calls.
      Remaining: the YCbCr decode path (`sceMpegAvcDecodeYCbCr`), which FU does not import, and seeking.
- [x] Savedata (`sceUtility`) save/load/list/delete/sizes to plain folders, `homebrew/savetest` (2026-10-16).
- [x] Our own UI for the save list, message dialog and on-screen keyboard (`05` §5h, 2026-10-18): drawn over the game,
      gamepad and physical keyboard; verified in FU's character creation (name typed, autosave toast).
- [ ] GE: stencil, color test, logic ops, CPU reads of framebuffers; Bézier/spline patches if used (gaps in `05` §6).
- [ ] Overlays: stage and monster overlays dispatch correctly.
- [ ] Village, shop, Felyne kitchen, one gathering quest and one hunting quest completed.
- [ ] Performance measured on a modest PC and on Linux.
- [x] Owner playtest (2026-10-21): single player played from start to finish on Windows. Fixed in 0.2.1: audio clicks (the
      output device was fed 0.2 % too little, `psp_frontend.cpp`) and flicker of item icons, text and textures at 60 fps and
      above (2D sprites are paired strictly, `05` §6). Fixed in 0.2.2: that pairing ignored the alternating draw buffer, so
      the 2D HUD did not interpolate and the Linux interpolation test failed.
      Known: 180 fps not sustained in the heaviest areas, saves are in the PSP format (import from a PSP or PPSSPP in Settings; usable in PPSSPP).
**Acceptance**: complete 3 quests and save/load without failures; stable framerate matching the original.

## Phase 4 — Launcher and packaging

- [x] Gamepad-first launcher (`07` §3b, 2026-10-17): React/Motion UI in the system web view, add copy with SHA-1
  verification, settings passed to the game, play time; friends/rooms UI waits for online play.
- [x] Installer scaffolding (`07` §3b, 2026-10-19): per-user data folder, image checks, optional image copy, `MHForever --install`,
  executable identity gate (size + SHA-1 in, SHA-1 out). Tests on a synthetic image, no game data.
- [x] Executable preparation module: the decryption step behind `builtin_decryptor()` (PPSSPP's PRX decrypter, GPL), validated
  byte for byte against `[dev_executable]` with the developer's disc (2026-10-19).
- [x] Saves (`<Documents>/MHForever/saves/<DISC_ID>`) and `launcher.json` (`%APPDATA%/MHForever`) kept outside the program folder,
      with a one-time copy from the old layout (0.2.1, 2026-10-22).
- [ ] Launcher: language selection, signed updates.
- [x] Windows zip with `scripts/package_release.py`; v0.1.0 published (2026-10-19), v0.2.0 adds frame interpolation (2026-10-21), v0.2.1 fixes audio clicks and keeps saves and settings outside the program folder (2026-10-22), v0.2.2 makes the 2D HUD interpolate again (2026-10-22), v0.2.3 makes interpolation lighter (about 90 to 150 fps in
      the village at 180, 2026-10-22), v0.3.0 adds the in-game menu on Escape with live video and audio settings and
      internal resolution up to 10x (2026-10-22), v0.4.0 uses the PSP save format: saves can be imported from a PSP or PPSSPP
      and copied back (2026-10-22), v0.4.1 is the first release with a Linux tarball next to the Windows zip (2026-10-23).
- [x] Linux release (2026-10-22): portable `MHForever-<version>-linux-x64.tar.gz` from
      `scripts/package_release.py --platform linux` (programs, fonts, licenses, `dist/PLAYER_README_LINUX.txt`), embedded
      application icon, and a `cmake --install` layout with the desktop entry and the icon. CI builds and packages the layout
      on Ubuntu.
- [ ] Windows installer and AppImage (Linux). Deck testing by friends.
**Acceptance**: a user with no development tools goes from "I have my ISO" to "I'm playing" using only the launcher.

## Phase 5 — Multiplayer

- The network layer is shared by every future console (3DS, Switch, PS2, Wii, Wii U): `core/net` offers rooms
  (packets or inputs mode), each console adds an adapter (`08-multiplayer.md` §1.1).
- [x] Harden HLE boundaries first (`08-multiplayer.md` §4.6): the Windows drive-letter path escape is fixed,
  `host0:` is closed by default, and the GE and HLE guest memory bounds were checked (2026-10-23). Fuzzing comes with the
  net HLE.
- First transport: the community's ad hoc servers (PPSSPP/aemu), decided 2026-10-23 (`08-multiplayer.md` §1.2).
- [ ] Vendor the aemu_postoffice client; PSP adapter (`sceNetAdhoc*`, `sceNetAdhocctl*`, `sceNetAdhocDiscover*`) with
  tracing; adhoctest homebrew against a local reference server.
- [ ] Two instances on one PC complete a quest through a local server.
- [ ] Launcher: server choice and live status, stable virtual MAC, pseudonym; play on a community server. Credits, and
  messages to PPSSPP and the operators, before release. **Target: 0.5.**
- [ ] Then the target design on top (`08-multiplayer.md` §4): identity, friends, presence, invites, room codes; optional
  direct P2P links.
**Acceptance**: 4 players (Windows + Linux/Deck, different networks) complete a quest together.

## Phase 6 — US and JP regions

- [ ] `symbols.toml` and `overlays.toml` for ULUS10391 and ULJM05500; symbol-coverage test.
**Acceptance**: all three regions meet phase 3 criteria.

## Phase 7 — Optional enhancements

Done: internal resolution (`--scale`, launcher setting), bilinear texture option, right stick as D-pad camera, fullscreen toggle.
Done too: 60 to 180 fps by frame interpolation (`--fps`, launcher setting; `05-runtime-hle.md` §6, v0.2.0).
Done too: an in-game menu on Escape (pause, blurred full-screen backdrop, tabs, mouse, controller) that changes the volumes,
internal resolution (1x to 10x), frame rate, fullscreen and texture smoothing while playing (`07-launcher-ux.md` §3c).
Done too: the keyboard bindings are chosen in the launcher (Settings → Controls; `07-launcher-ux.md` §5), and so are the
controller's buttons and stick; both can also be changed from the in-game menu (Controls tab) and are kept (2026-10-23).
Pending: widescreen, analog camera hook, per-controller profiles, per-controller icons, vsync, and in-between
pictures made on a thread of their own:
the GE runs the game's display lists (about 10 ms per frame in heavy scenes) on the same thread, which caps 180 fps at
about 150 there (measured with `MHF_PROFILE=1`, 0.2.3). See `06-mhfu-module.md` §9.

## Phase 8 — Next game

Portable 3rd or Freedom 2/Freedom (PSP), reusing the runtime. Then PS2 (Monster Hunter 1/G/2 "Dos") once PS2Recomp is usable, and
3DS (MH4U, Generations) once a base exists.

## Backlog / ideas

- Import PSP/PPSSPP saves (careful: save encryption).
- Local "download server" for event quests the player already owns (`05` §1, P3).
- Recording, voice chat.
- macOS/ARM64 ports.
- Clean, progressive reimplementation of modules (camera, UI, network) to depend less on translated code.
