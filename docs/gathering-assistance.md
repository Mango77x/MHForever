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

## Icons

Without a texture pack, icons are decoded at startup from the original HUD and item atlases in the player's own ISO.
Nothing is extracted to disk or distributed. The existing read-only ISO/DATA.BIN helpers locate the native TMH images,
using file IDs from the active region map. The native decoder accepts only the expected 256x256 CLUT8/RGBA8888 images.
If they cannot be read, the indicator falls back to English or Spanish text.

With a texture pack enabled, the program reads icons directly from that selected folder. The existing pack reader
chooses the release's ini, hashes the original atlases with the pack's configured rules, and resolves their replacement
images through the mapping. PNG and KTX2 atlases are cropped in memory at the original coordinates, scaled to the
replacement's resolution. No extraction step or `gathering-icons/` folder is used by the game.

Switching the pack in the launcher takes effect on the next game launch, as it does for the game's other textures.
Each atlas falls back independently to the original: a pack may replace the HUD but keep the original item icons.
Missing, corrupt or incompatible images, an explicit keep-original mapping, and unsupported address-only mappings
also use the originals. Content/palette mappings and the original square atlas layout are required; an arbitrary
redesign of the atlas cannot be identified from its dimensions alone.

The HUD atlas contains Circle at grid (0,0) and the hexagon at (220,224); the items atlas contains pickaxe (24,48), net
(144,24), and question mark (48,96), on a 256-unit grid. Carving uses the generic question-mark indicator.
No game artwork is distributed.

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

Tests cover selected-pack switching, replacement scaling/alpha, per-atlas original fallback, native swizzled atlas decoding, premultiplied alpha, truncated/unsupported image rejection, tool selection, depleted nodes, dynamic carving targets, region/position filters, guest-context preservation,
failed-interaction rollback, menu input latching, system dialogs, icon mapping, red X rendering and idle hiding.
Carving targets are read without calling reward or consumption functions. All addresses come from the region map.

## Attribution

The interaction idea, original atlas mapping and placement are inspired by Lunng's
[Gathering Nodes mod](https://www.nexusmods.com/monsterhunterfreedomunite/mods/26) and its
[source](https://github.com/pggkun/mhfu_gathering_nodes). This implementation uses native host code; no original injected
MIPS code or game assets are included.
