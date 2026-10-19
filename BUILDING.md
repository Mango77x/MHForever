# Building MHForever from source

Requirements: CMake ≥ 3.20, Ninja, Clang (Linux) or Visual Studio 2022 with clang-cl (Windows), Python 3 for repository checks.

```sh
cmake --preset linux-clang        # or windows-clang / windows-msvc
cmake --build --preset linux-clang
ctest --preset linux-clang
```

The PSP test programs in `homebrew/` are built and run as tests when the [pspdev](https://github.com/pspdev/pspdev)
toolchain is installed (`PSPDEV` environment variable or `/opt/pspdev`). The SDL3 window/input/audio host is fetched and
built automatically (`-DMHF_WITH_SDL=OFF` to skip it).

## Developer build of Freedom Unite (local only)

This uses your own copy of the game and never produces anything that may be committed or shared
(see `CONTRIBUTING.md`).

1. Keep your ISO at hand. Players get the game's executable prepared by the launcher's installer (`launcher --install <your.iso>`,
   see `docs/07-launcher-ux.md` §3b); developers can instead point the tools at a prepared executable (`docs/06-mhfu-module.md` §3).
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

See [CONTRIBUTING.md](CONTRIBUTING.md) for the rules every change must follow.
