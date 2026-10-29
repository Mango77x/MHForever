# Building MHForever from source

Requirements: CMake ≥ 3.20, Ninja, Clang (Linux) or Visual Studio 2022 with clang-cl (Windows), Python 3 for repository checks.
The launcher also needs Node.js/npm to build its UI (pass `-DMHF_BUILD_LAUNCHER=OFF` to skip the launcher). On Linux it needs the
WebKitGTK 4.1 and GTK 3 development packages, and SDL3 looks for the audio, OpenGL and input ones. The CI installs:
`ninja-build clang pkg-config libwebkit2gtk-4.1-dev libgtk-3-dev libasound2-dev libgl1-mesa-dev libxkbcommon-dev libwayland-dev`.

```sh
cmake --preset linux-clang        # or windows-clang / windows-msvc
cmake --build --preset linux-clang
ctest --preset linux-clang
```

The PSP test programs in `homebrew/` are built and run as tests when the [pspdev](https://github.com/pspdev/pspdev)
toolchain is installed (`PSPDEV` environment variable or `/opt/pspdev`). The SDL3 window/input/audio host is fetched and
built automatically (`-DMHF_WITH_SDL=OFF` to skip it).

FFmpeg is mandatory (it decodes the movies and the game's music). On Linux the build uses the FFmpeg the system already
has, through pkg-config: install its development files first (`libavcodec-dev` and `libavutil-dev` on Debian/Ubuntu,
`ffmpeg` on Arch). `-DMHF_FFMPEG=bundled` downloads a pinned release, checks its SHA-256 and builds it once, linked
statically, which is what a self-contained release archive does; Windows defaults to it. There is no option without
FFmpeg.

## Running on Linux

The release tarball is a portable folder: extract it and run `./MHForever`. The launcher needs WebKitGTK 4.1 and GTK 3 at
runtime (the build packages above are enough if you build from source on the same machine). A typical Debian/Ubuntu system
needs:

```sh
sudo apt install libwebkit2gtk-4.1-0 libgtk-3-0 libasound2t64 libgl1
```

On Fedora use `webkit2gtk4.1 gtk3 alsa-lib mesa-libGL`, and on Arch `webkit2gtk-4.1 gtk3 alsa-lib mesa`. X11 and Wayland
are both supported; the game needs a driver with OpenGL 3.3. SDL3 itself is linked into the programs.

To install a local build for the current user (launcher, game when it was built, fonts, desktop entry and icon):

```sh
cmake --install build/linux-clang --prefix ~/.local
```

## Developer build of Freedom Unite (local only)

This uses your own copy of the game and never produces anything that may be committed or shared
(see `CONTRIBUTING.md`).

1. Keep your ISO at hand. Players get the game's executable prepared by the launcher's installer (`MHForever --install <your.iso>`,
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
   Every region has its own program, because the recompiled code is the release's own. The European one is `mhfu`; for
   North America generate from `ULUS10391` and configure with `-DMHF_MHFU_PROGRAM_NAME=mhfu-us`, for Japan from
   `ULJM05500` with `-DMHF_MHFU_PROGRAM_NAME=mhfu-jp`, so the launcher finds it (it starts `mhfu`, `mhfu-us` or `mhfu-jp`
   by the disc in the library).
4. Run it: `build/game/games/mhfu/mhfu --iso <your.iso> --exe <decrypted EBOOT> --window`
   (`--dump-frames <dir>` saves PNGs of the screen, `--trace` logs every system call, `--seconds N` stops after N s).

Keyboard: arrows = D-pad, WASD = analog stick, K/L/J/I = cross/circle/square/triangle, Q/E = L/R, Enter = Start,
Backspace = Select. The launcher's Settings → Controls changes them, and `--keymap "south=KeyK,east=KeyL,..."` does the
same from the command line (`--padmap` for the controller's buttons). Any controller SDL recognizes works (Xbox layout: A = cross, B = circle, X = square, Y = triangle).

### Trying online play (developers)

Online play talks to a PSP ad hoc server (PPSSPP's and aemu's protocol, `docs/08-multiplayer.md` section 1.2). It is off unless a server
is set, and nothing is contacted before the game first uses the ad hoc libraries:

- `mhfu ... --adhoc-server host[:port] [--adhoc-nick <name>] [--adhoc-mac AA:BB:CC:DD:EE:FF]`, or the environment variable
  `MHF_ADHOC_SERVER`; the launcher passes them from Settings → Online.
- `MHF_TRACE_ADHOC=1` logs the ad hoc calls and, once a second, the datagrams read and sent; `=2` also dumps the start of every datagram.
- A local server to test against: build aemu_postoffice's reference server (commit in `third_party/aemu_postoffice/NOTICE.md`, directory
  `server_cpp`, C++20, clang or g++ on Linux or WSL) and run it. Its adhocctl port is 27312, the relay 27313, and `http://127.0.0.1:8888/data.json`
  shows who is connected (its log file is buffered; read the status page instead). In WSL2 build it with `steady_clock` in place of
  `high_resolution_clock`, or it drops every client when the clock is stepped (`docs/05-runtime-hle.md` section 5i).
- `MHF_ADHOC_TEST_SERVER=<host>` makes the test `psp_adhocctl_server` run two players against that server (datagrams, a stream, optionally
  `MHF_ADHOC_TEST_KEEPALIVE=1` and `MHF_ADHOC_TEST_JITTER=1`); without it the test is skipped.
- A second copy of the game on the same machine: run `mhfu.exe` again with another `--memstick` (a copy of the saves folder), `--name`,
  `--adhoc-nick` and `--adhoc-mac`, both pointing at the same server and entering the same room.
- PPSSPP as the other end: network on, the server's address set, and its "Try to use the packet relay of the server" setting forced on,
  changed with no game loaded (for a server that is not in PPSSPP's published list).

Enable the content guard once per clone:

```sh
git config core.hooksPath .githooks
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the rules every change must follow.

## Making a release

The archives are assembled from programs built elsewhere; game data never goes into them.

### Windows

1. Build the launcher on Windows (`cmake --preset windows-msvc`, then `cmake --build --preset windows-msvc`). The C/C++
   runtime is linked statically, so the launcher runs on a clean Windows install.
2. Build `mhfu.exe` from the generated code (see above) in a Release configuration.
3. Package them with the free fonts and the licenses:
   ```sh
   python scripts/package_release.py --launcher <MHForever.exe> --game <mhfu.exe> --fonts <the fonts folder next to mhfu.exe> \
       --ffmpeg-dlls <build folder>/bin
   ```
   This writes `MHForever-<version>-windows-x64.zip` and its `.sha256`. The text shown to players is `dist/PLAYER_README.txt`.
   `--ffmpeg-dlls` is the folder CMake copies FFmpeg's DLLs to (`<build folder>/bin`); they go next to `mhfu.exe`, because
   the game loads them from its own folder and a clean Windows install has none. The script refuses to build the
   archive without them. The FFmpeg licence and source note (`third_party/ffmpeg/`) go into the archive as they are.
   `--game-us <mhfu-us.exe>` and `--game-jp <mhfu-jp.exe>` add the North America and Japan programs to the same archive.
   `--version` is optional: it defaults to `project(MHForever VERSION ...)` in `CMakeLists.txt`, which is the only place
   the version is written, so the archive, the launcher and `mhf --version` always agree. Bump it there (and only there)
   when preparing a release; pass `--version` only for a one-off archive.

### Linux

1. Build the launcher (`cmake --preset linux-clang`, then `cmake --build --preset linux-clang`).
2. Build `mhfu` from the generated code (see above) in a Release configuration, with `-DMHF_FFMPEG=bundled` so FFmpeg is
   built from the pinned release and linked statically: the archive then needs nothing from the player's distribution.
3. Package the programs, the free fonts and the licenses:
   ```sh
   python scripts/package_release.py --platform linux \
       --launcher <MHForever> --game <mhfu> --game-us <mhfu-us> --game-jp <mhfu-jp> --fonts <the fonts folder next to mhfu>
   ```
   This writes `MHForever-<version>-linux-x64.tar.gz` and its `.sha256`; the execute permission of the programs is
   kept. FFmpeg is inside the programs (static), so the archive carries only its licence and source note
   (`third_party/ffmpeg/`). The text shown to players is `dist/PLAYER_README_LINUX.txt`. As on Windows, the version comes from
   `CMakeLists.txt`.

Release archives are always built by the maintainer on their own machine (for Linux: on a Linux install, a virtual
machine or WSL), never by CI and never by a third party, because they are programs people will run. CI only builds and
tests, and checks that the Linux archive layout still packages (it uses the core program as a stand-in for the game).
