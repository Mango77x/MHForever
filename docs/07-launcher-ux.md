# 07 — Launcher, user experience, controllers and Steam Deck

## 1. Principles

- **Gamepad-first**: the whole launcher works with a controller (the Deck has no comfortable keyboard). Mouse and keyboard too.
- Fullscreen by default on Deck; resizable window on desktop.
- No jargon: "Add my copy", "Play", "Play with friends".
- No art from the series in the launcher UI, except optionally the icon read from the player's copy (pending legal OK).

## 2. First-run flow

1. Welcome + notice: "You need your own copy of the game. This program contains and downloads none of the game's assets."
2. **Add game** → file picker (on Deck, SDL3's dialog / desktop portal; also "scan this folder").
3. Hash identification → shows "Freedom Unite · Europe (ULES01213 v1.01) ✓" or an explained error.
4. **Prepare**: the installer checks the image, prepares the game's executable from it and, if the player wants, keeps a copy of
   the image, with a progress bar (§3b). The player never recompiles anything (model A); only model B would add local
   recompilation and compilation.
5. Play.

## 3. Screens

Library (cards per game/region), global settings (video, audio, controllers, network, language), per-game settings (toggleable
enhancements), multiplayer (create room / join with code), diagnostics (logs, "copy report").

## 3b. Implementation (2026-10-17)

The launcher is `launcher/`: a native host plus a web UI shown in the **system web view** (WebView2 on Windows,
WebKitGTK on Linux/Deck) through the header-only `webview` library (MIT). Qt was ruled out when the repository was GPL-2.0-only (it is GPL-3.0-or-later now); Dear ImGui could not reach the target visual/motion quality.

- **UI** (`launcher/ui`): React + TypeScript + Motion (spring animations, shared-layout transitions), Sora/Manrope fonts
  (OFL) bundled. `vite build` produces one self-contained `index.html` (scripts, styles and fonts inlined) that CMake
  embeds into the executable (`launcher/embed.cmake`), so the launcher never loads anything from the network. In a plain
  browser (`npm run dev`) a simulated backend in `bridge.ts` stands in for the native side.
- **Bridge**: native functions `mhf_getState`, `mhf_setSettings`, `mhf_setPseudonym`, `mhf_addGame`, `mhf_removeGame`,
  `mhf_pickExecutable`, `mhf_play`, `mhf_copyText`, `mhf_exportIdentity` (Promises of JSON); events pushed with
  `window.__mhfEvent` (`verify`, `verified`, `verifyFailed`, `launch`, `exited`, `launchFailed`, `state`).
- **Native host** (`launcher/native`): `launcher.cpp` keeps `launcher.json` next to the executable (portable for now):
  pseudonym, identity seed, settings, library, stats. Adding a copy reads PARAM.SFO (`platforms/psp/tools/param_sfo`)
  and compares size + SHA-1 with `games/*/regions/*/region.toml` (catalog generated at configure time; identification
  data only). Launching runs `mhfu(.exe)` next to the launcher with the settings as flags (`--scale`, `--fullscreen`,
  `--smooth-textures`, `--fps`, `--music-volume`, `--sfx-volume`, `--name`), logs to `game.log`, and records play time and
  sessions. The game also needs the prepared executable: the installer produces it (below); developer builds can instead use
  `../exe/<DISC_ID>_EBOOT.BIN` (or one next to the ISO), checked against `dev_executable` in region.toml.
- **Friend code**: generated locally (SHA-256 of a random identity seed, Crockford base32). Provisional: online play will
  derive it from the Ed25519 public key (`08-multiplayer.md` §4.1); the backup file carries the seed.
- **Input**: mouse, keyboard (arrows/Enter/Esc) and gamepad (Gamepad API: D-pad/stick move focus spatially, A selects,
  B goes back; menus, drawers and dialogs trap focus via `data-nav-scope`). `prefers-reduced-motion` is honoured.
- **Windows details**: dark title bar (DWM), WebView2 background colour set before creation (no white flash), message
  if the WebView2 runtime is missing; the game starts without a console window.
- Tests: `launcher_core` (hashes, JSON, PARAM.SFO, settings, identity persistence, `launcher.json` surviving a failed write;
  with `MHF_TEST_ISO` it verifies a real copy, never in CI) and `launcher_install` (installer on a synthetic image). The UI was checked with screenshots in Chromium and WebKitGTK.
- **Installer (2026-10-19)**, modelled on the first-run installer of TeamGDB/Yakumo (its issues #26/#27/#28, PR #35): adding
  a copy also *sets the game up* from the image, so a player needs nothing but the disc image. Code: `launcher/native/install/`.
  - *Per-user data* (`data_dir`): `MHF_DATA_DIR`, else `%APPDATA%/MHForever` or `$XDG_DATA_HOME/MHForever`
    (`~/.local/share/MHForever`); layout `games/<DISC_ID>/executable.elf` and, when the image is copied, `image.iso`.
    Executable lookup order: the path set in Settings, the per-user folder, then the developer layout (`../exe`, next to the ISO).
    `launcher.json` and logs stay next to the launcher for now; saves live in `<Documents>/MHForever/saves` (`install::user_saves_dir()`, `MHF_SAVES_DIR` overrides), imported once from the old `<launcher>/memstick`.
  - *Executable preparation* (`executable_preparation`): reads `PSP_GAME/SYSDIR/EBOOT.BIN` from the image, accepts it only if
    its size and SHA-1 equal `[encrypted_executable]` in `region.toml`, then refuses any output whose SHA-1 is not
    `[dev_executable]`'s. The decryption itself is a replaceable `Decryptor`; `builtin_decryptor()` (`prx_decryptor.cpp`) wraps PPSSPP's PRX
    decrypter (`third_party/ppsspp_prx`) and rebuilds the signed-ELF layout (plain ELF + `FFEEDDCC` marker + the last bytes of
    the original file). Verified on the EU disc: output is byte for byte `[dev_executable]` (SHA-1 `04b60ee7…`).
  - *Setup flow* (`installer`): checks the image (compressed CSO/ZSO, not an image, another revision, damaged), prepares the
    executable, and with the **Keep a copy of my disc image** setting checks free space and copies it under a temporary name
    with progress. Nothing partial is left behind on failure. Removing a game deletes only what the installer stored.
  - `MHForever --install <image.iso> [--in-place | --copy]` runs the same setup without the window (Windows borrows the
    parent console).
- **Release packaging (2026-10-19)**: `scripts/package_release.py` assembles the Windows zip (launcher, game program, free fonts,
  licenses and `dist/PLAYER_README.txt`) and refuses anything that looks like game data. 0.1.0 and 0.2.0 were published this way.
- Still to do: launcher-managed updates (signed), language selection, icon, Steam Deck packaging
  (AppImage/Flatpak), moving the per-user folder, moving saves there.

## 4. Bundled toolchain (only for model B; not needed with model A)

| Option | Approx. size | Pros | Cons |
|---|---|---|---|
| **`zig cc`** | ~41–50 MiB (Linux) | Embedded Clang, libc shipped as sources, static binary, cross-platform | Depends on Zig versions; validate with C++20 |
| Minimal LLVM/Clang + lld + headers | 100–400 MiB | Full control | Heavy; Windows needs SDK/CRT |
| Ask the user to install a compiler | 0 | Simple | Unacceptable for "anyone" and for Deck |
| Compile in the cloud | 0 client-side | Fast | We would effectively distribute the derivative: **rejected** |

## 5. Input (`core/input`)

- **SDL3 Gamepad API**. Load `gamecontrollerdb.txt` (community database) with `SDL_AddGamepadMappingsFromFile` for unusual pads.
- **Steam Input**: when the player adds the launcher to Steam as a non-Steam game, Steam exposes a virtual pad (XInput-like). We
  must work well with it: do not fight `SDL_GAMECONTROLLER_IGNORE_DEVICES`, show generic glyphs if the type is unknown. Gyro through
  Steam Input has known limitations in SDL; do not depend on it.
- **Default PSP → modern controller mapping** (Xbox / PlayStation):
  - Left stick → PSP analog. D-pad → D-pad.
  - Face buttons: △/Y, ○/B, ×/A, □/X by physical position (option to swap confirm/cancel per region).
  - L/R → LB/RB (LT/RT configurable too).
  - START, SELECT → Menu/View.
  - **Right stick → D-pad** (FU's "claw" camera) with configurable deadzone and repeat. Level 1 is implemented; level 2 (analog camera hook) is an enhancement.
- Keyboard and mouse: sensible defaults; the mouse drives the camera once the analog hook exists.
- Full remapping per game, per-controller profiles, deadzones, sensitivity; rumble optional (the PSP has none).
- Several local pads: player 1 only (the PSP is single-player). Local multiplayer on one machine = several instances: out of scope v1.

## 6. Steam Deck / Linux

- Native Linux x86-64 build (do not rely on Proton).
- Distribution: **AppImage** first; Flatpak later (check Flathub content policies).
- Deck resolution 1280×800 (16:10); PSP 480×272 (~16:9). Integer scaling (×2 = 960×544) or fitted with a filter, crop option, and
  widescreen as an enhancement.
- Test: suspend/resume, Game Mode input, performance and battery.
- Real testing: maintainers have no Deck; friends with Decks test builds already validated on a Linux machine. They need their own copy
  to test the game; for the rest (input, launcher, scaling) use the test scene built from our own homebrew.

## 7. User data

- Windows: `%APPDATA%/MHForever/`. Linux: `$XDG_DATA_HOME/MHForever/` (`~/.local/share/MHForever`).
- Today the per-user folder holds `games/<DISC_ID>/executable.elf` (and `image.iso` when a copy is kept); `launcher.json`,
  `game.log` stays next to the launcher (§3b); the saves are in `<Documents>/MHForever/saves`.
- Target: `saves/`, `logs/` and `config.toml` there too, and an option to move the folder.
