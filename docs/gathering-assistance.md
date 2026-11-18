# Gathering assistance (Freedom Unite / Portable 2nd G)

Two independent, opt-in enhancements are available in the native game programs:

- `--gathering-indicators`: show nearby gathering, mining, bug-catching and carving targets.
- `--auto-gathering-tools`: Circle selects a suitable pickaxe or bug net and requests the game's normal interaction.

Both are off by default. The Game tab can toggle them for the current session. Supported releases are:

| Release | Disc ID | Native program |
| --- | --- | --- |
| Europe | `ULES01213` | `mhfu` |
| North America | `ULUS10391` | `mhfu-us` |
| Japan (Portable 2nd G) | `ULJM05500` | `mhfu-jp` |
| Japan + FUComplete v1.4.0 | `ULJM05500-FUC` | `mhfu-fuc` |

The target layouts, tool IDs and action codes are shared; each release resolves addresses through its own region map.
Unmapped releases do nothing. Launcher settings and persistent launcher integration are outside
this change. Stock 2nd G retains its Japanese text; FUComplete uses its English translation. The optional indicator
text fallback is English or Spanish.

FUComplete is prepared separately with its [official patcher](https://fucomplete.github.io/docs/setup.html) from a copy
of the player's Japanese image. MHForever does not patch it. Its gathering functions, data layout and frame-hook
boundary match stock 2nd G; its hold-to-gather changes run through the game's own interaction routines.

The indicator appears near the upper centre and disappears when there is no nearby target. Nearby exhausted targets
and targets missing a required tool keep the action icon with a red X instead of Circle. Menus and system dialogs
suppress interaction; closing the host menu with Circle does not trigger gathering. Item consumption and animations
remain the game's responsibility. Gathering/carving itself uses the game's normal Circle input.

## Optional icons

Without local icons, the indicator falls back to English or Spanish text. No game artwork is distributed.
To reproduce the original-style appearance, use compatible HUD and item atlases from your own texture installation.
The preparation tool accepts KTX2 or PNG directly; no separate decoder or Python image library is required:

```sh
cmake --build build/linux-clang --target mhfu_prepare_gathering_icons
build/linux-clang/games/mhfu/mhfu_prepare_gathering_icons \
    /path/to/your/hud.ktx2 /path/to/your/items.ktx2 /path/to/game-folder/gathering-icons
```

Run the corresponding native program with that game folder as its working directory. It reads five files from `gathering-icons/`:
`background.rgba`, `circle.rgba`, `mining.rgba`, `bugs.rgba`, and `gather.rgba`. Each contains an eight-byte `MHFICON1`
header, a little-endian u32 square dimension, and premultiplied RGBA8 pixels. Missing or invalid files use text instead.
The tool uses the original 256-unit atlas coordinates and supports higher-resolution replacements.

With a KHD 1.3 installation, the compatible atlases are `ui/hud/ddeab148_fu.ktx2` and
`ui/icons/items/fc6e32c0.ktx2`. These names identify local inputs; the files must not be attached to the contribution.
Other packs must retain the game's original HUD/items atlas layout. Find the pack's texture mapping first and inspect
its candidate HUD/items atlas locally; matching dimensions alone do not establish compatibility. The HUD atlas must
contain Circle at grid (0,0) and the hexagon at (220,224); the items atlas must contain pickaxe (24,48), net (144,24),
and question mark (48,96). There is no automatic atlas discovery for arbitrary packs.
The original mapping uses a pickaxe, bug net, and generic question mark; carving uses the generic indicator.

## Building and verification

Follow the normal developer preparation in `docs/04-recompiler.md`: regenerate each desired program with its updated
region map before building the game (`prepare.py --region ULES01213`, `ULUS10391`, `ULJM05500`, or `ULJM05500-FUC`). Existing generated output from before this change lacks the gathering hook.
The developer's own executable/ISO and generated files stay in ignored `user_data/` directories.

The runtime tests generate headers directly from the region maps and run the same synthetic scenario for all four
editions, plus a separate unmapped case. They do not require `MHF_MHFU_GENERATED` or game data. The remaining
tests also use synthetic memory, guest functions and pixels:

```sh
cmake --build build/linux-clang
ctest --test-dir build/linux-clang --output-on-failure
```

Tests cover tool selection, depleted nodes, dynamic carving targets, region/position filters, guest-context preservation,
failed-interaction rollback, menu input latching, system dialogs, icon mapping, red X rendering and idle hiding.
Carving targets are read without calling reward or consumption functions. All addresses come from the region map.

## Attribution

The interaction idea, original atlas mapping and placement are inspired by Lunng's
[Gathering Nodes mod](https://www.nexusmods.com/monsterhunterfreedomunite/mods/26) and its
[source](https://github.com/pggkun/mhfu_gathering_nodes). This implementation uses native host code; no original injected
MIPS code or game assets are included.
