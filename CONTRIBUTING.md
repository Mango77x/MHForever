# Contributing to MHForever

## Pull requests

Pull requests are welcome. MHForever has a single maintainer, who reviews every change and is the only one who merges.
There is no guarantee that a pull request will be accepted or answered quickly. For anything large, open an issue first
so the work is not wasted.

Bug reports are welcome as issues. Never attach game files, saves, screenshots of the game or logs that contain game data
(see the rules below). You are free to fork the project under the terms of the license.

## Rules

MHForever is an unofficial, non-commercial project. These rules keep the repository clean and are non-negotiable for
everyone who works on it. If a change would break one of them, open an issue instead of a pull request.

## Content rules

1. **Never** add to the repository, a pull request, an issue or a public log: game discs or images, executables
   (encrypted or decrypted), game archives, textures, models, audio, video, game text, saves, game screenshots or
   memory dumps. Identification hashes (disc IDs, sizes, SHA-1/SHA-256) are allowed.
2. **Never** commit code generated from a game executable. Recompiler output lives in git-ignored folders.
3. The game's executable is prepared on the player's machine, from the player's own disc image, by the installer
   (`launcher/native/install/`). That module accepts only the executables listed in `games/*/regions/*/region.toml`
   (size and SHA-1), refuses any result that does not match the recorded SHA-1, and is used only by the installer. Its decryption comes from
   PPSSPP's PRX decrypter and libkirk (`third_party/ppsspp_prx/`, GPL-compatible, see `THIRD_PARTY.md`). Do not add
   any other DRM-related code, and never commit its inputs or outputs.
4. Trademarks: the names and logos of the games and their publishers are not used as the name of the product,
   the repository, the executables, the icons or the UI. Descriptive compatibility statements in plain text are fine
   ("works with your copy of ...").
5. Third-party code must have a license compatible with GPL-3.0-or-later. Put it in `third_party/<name>/` with its
   license and add an entry to `THIRD_PARTY.md`.
6. Proprietary system fonts are not included. Use free fonts.
7. No monetization: no donation or payment links in the software or the repository.

## Working rules

- Everything is in English: code, comments, documentation, commit messages and issues.
- Code specific to one game lives in `games/<id>/`. The core (`core/`, `platforms/psp/`) contains no game-specific
  addresses, offsets or names.
- Never hardcode a game address: resolve it by symbol name through the active region's map.
- Tests must run without game data. Tests that need a game are tagged `requires_game_data` and never run in CI.
- Enhancements (widescreen, higher frame rate, analog camera) are opt-in and never mixed with fidelity fixes.
- Do not paste disassembly or decompiled code of a game into the repository, issues or documentation.
- Small commits with messages of the form `area: change`.
- Before sending a change: build, run the tests and update `docs/10-roadmap.md` when a milestone moves.
- Enable the content guard once per clone: `git config core.hooksPath .githooks`.

## Documentation

See `docs/` (architecture, recompiler, runtime, launcher, multiplayer, roadmap).
