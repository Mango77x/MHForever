# MHForever

**An unofficial Monster Hunter recompilation project.**

> Not affiliated with, endorsed by or sponsored by Capcom or Sony. Requires your own legitimate copy of the game.
> No game content is included or downloaded. "Monster Hunter" is a trademark of its owner and is used here only to
> describe which games the project works with.

A platform to play classic games of the series **natively on PC** (Windows, Linux and Steam Deck), with any controller
and with friends over the internet, starting with **Freedom Unite (PSP)**.

- Not an emulator: the game's code is **statically recompiled** to native code.
- **Contains no game content and downloads none.** Every player needs their own copy; all game data is read from it.
- Status: early development. Freedom Unite boots and plays; packaging and a first public release are in progress.
- Developed with AI assistance.

## Documentation

| | |
|---|---|
| [Overview](docs/00-overview.md) | [Contributing](CONTRIBUTING.md) |
| [Architecture](docs/02-architecture.md) | [PSP platform](docs/03-psp-platform.md) |
| [Recompiler](docs/04-recompiler.md) | [Runtime / HLE](docs/05-runtime-hle.md) |
| [Freedom Unite module](docs/06-mhfu-module.md) | [Launcher, controllers, Steam Deck](docs/07-launcher-ux.md) |
| [Multiplayer](docs/08-multiplayer.md) | [Roadmap](docs/10-roadmap.md) |

## Building

Requirements: CMake ≥ 3.20, Ninja, Clang (Linux) or Visual Studio 2022 with clang-cl (Windows), Python 3 for repository checks.

```sh
cmake --preset linux-clang        # or windows-clang / windows-msvc
cmake --build --preset linux-clang
ctest --preset linux-clang
```

The PSP test programs in `homebrew/` are built and run as tests when the [pspdev](https://github.com/pspdev/pspdev)
toolchain is installed (`PSPDEV` environment variable or `/opt/pspdev`). The SDL3 window/input/audio host is fetched and
built automatically (`-DMHF_WITH_SDL=OFF` to skip it).

### Developer build of Freedom Unite (local only)

This uses your own copy of the game and never produces anything that may be committed or shared
(see `CONTRIBUTING.md`).

1. Prepare the decrypted executable of your own copy (`docs/06-mhfu-module.md` §3) and keep your ISO at hand.
2. Build the tools (`cmake --build --preset <preset>`), then generate the code:
   ```sh
   python games/mhfu/tools/prepare.py --iso <your.iso> --exe <decrypted EBOOT> --tools build/<preset>
   ```
   Output goes to `user_data/generated/<DISC_ID>/` (git-ignored).
3. Configure a separate build with the generated folder and build the `mhfu` target (large: about 1.4 million recompiled
   instructions; use all cores):
   ```sh
   cmake --preset <preset> -B build/game -DMHF_MHFU_GENERATED=user_data/generated/ULES01213 -DMHF_BUILD_TESTS=OFF
   cmake --build build/game --target mhfu
   ```
4. Run it: `build/game/games/mhfu/mhfu --iso <your.iso> --exe <decrypted EBOOT> --window`
   (`--dump-frames <dir>` saves PNGs of the screen, `--trace` logs every system call, `--seconds N` stops after N s).

Keyboard: arrows = D-pad, WASD = analog stick, K/L/J/I = cross/circle/square/triangle, Q/E = L/R, Enter = Start,
Backspace = Select. Any controller SDL recognizes works (Xbox layout: A = cross, B = circle, X = square, Y = triangle).

Enable the content guard once per clone:

```sh
git config core.hooksPath .githooks
```

## License

GPL-2.0-only (see `LICENSE`). Third-party components keep their own licenses (see `THIRD_PARTY.md`).
