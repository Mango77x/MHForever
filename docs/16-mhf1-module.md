# Freedom module (`games/mhf1`)

Monster Hunter Freedom (PSP, 2005/2006; Monster Hunter Portable in Japan) as a game module of its own on the PSP
platform. Status: in development, European release only.

## 1. Where it sits

MHForever is layered so that every Monster Hunter game on the PSP gets the same treatment without depending on another
game:

| Layer | What goes there | Freedom uses |
|---|---|---|
| PSP platform (`platforms/psp`) | Runtime, system calls, recompiler, drawing (HD text, frame interpolation, texture packs, projected shadows, widescreen picture), audio, input, ad hoc play, launcher | All of it |
| Engine family | Code two or more games of one engine share | Nothing yet (see §4) |
| Game module (`games/<game>`) | Region maps, the game program, game-specific enhancements, tests | `games/mhf1` |

A game module never includes another game module's code.

## 2. The game

| | Europe |
|---|---|
| Disc id | ULES00318, version 1.01 |
| Languages | English, French, German, Spanish, Italian |
| Executable | 1,193,776 bytes encrypted, entry `0x0889AF1C`, loaded at `0x08804000` |
| `DATA.BIN` | 704,284,672 bytes, 5,874 files |
| Code overlays | 105 in `DATA.BIN` (13 tasks and subs, 92 stages); the monsters' code is in the executable |

The disc has the layout Freedom Unite's has: one `DATA.BIN` holding the data and the code overlays, no PRX modules.

## 3. Developer pipeline

1. Prepare the executable from your own disc image (the launcher's installer, or the developer's decrypter).
2. `python games/mhf1/tools/prepare.py --iso <image.iso> --exe <prepared executable> --tools <build dir>` extracts the
   overlays and recompiles everything to `user_data/generated/mhf1/<region>/` (git-ignored).
3. Configure a build with `-DMHF_GAMES=mhf1 -DMHF_MHF1_GENERATED=<that folder>` and build the `mhf1` target.
4. `mhf1 --iso <image.iso> --exe <prepared executable> --window`.

Measured on the European release: the recompiler covers 95.0 % of the executable's code and 82.8 % of the overlays'.

## 3.1 Freedom DX

A European image patched with Freedom DX v1.8.6 (SilverJolteon's patch, an xdelta) is its own copy, `ULES00318-FDX`
(program `mhf1-fdx`, `regions/ULES00318-FDX`). The image keeps the European disc id and adds no file, so the launcher
tells it by its executable: Freedom DX ships it plain (1193440 bytes), the release encrypted (1193776); the region's
marker is the executable, and identify() also checks its size. Freedom DX patches the executable in place (22 places,
its own code at 0x088C2140..0x088C338C) and leaves the mapped addresses unchanged, so the symbols are the release's.
Its hand-written code uses `addi`, which the decoder now runs as `addiu` (the overflow trap is not modelled). Coverage:
95.0 % of the executable, 82.8 % of the overlays.

The game reads its options and event quests from `PSP/SAVEDATA/FDXDAT` (CONFIG.BIN, EVENT.BIN), which comes with the
player's download, not on the disc: Game settings > Freedom DX copies the folder the player chooses into the copy's
saves. Its saves are the European release's (`ULES00318...` folders), which the save import looks for. Measured: the
title (Freedom DX v1.8.6), a save loaded and the village, at 30 fps.

## 4. Freedom and Freedom Unite

Freedom runs an earlier version of Freedom Unite's engine: 39 % of its functions are the same, instruction for
instruction (addresses aside), and `DATA.BIN` uses the same cipher, so Freedom Unite's overlay extractor reads it
unchanged. The addresses in `regions/ULES00318/symbols.toml` were found by matching Freedom Unite's mapped code.

What both games share is moved into an engine-family library only once Freedom runs, with Freedom Unite behaving
exactly as before; until then `prepare.py` calls the build's `mhfu_extract_overlays` and the game program repeats the
few lines of overlay and draw-wait handling it needs.
