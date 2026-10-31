# 06 — Freedom Unite (PSP) module

## 1. The game

- Expansion of Freedom 2. Released in Japan as **Portable 2nd G** on 2008-03-27; North America 2009-06-23; Europe 2009-06-26
  (Australia 2009-06-25).
- Local ad-hoc multiplayer for up to **4 players**. In 2009 Sony offered "adhocParty" (tunneling through a PS3) and FU was one of
  the first supported games.
- Western extras mentioned by the press: save transfer from Freedom 2 and sharing Felynes ("felyne casting"). `[VERIFY]`

## 2. Target regions

| Region | Disc ID | Title | Status |
|---|---|---|---|
| Europe | **ULES01213** | Freedom Unite | Main development (reference copy, v1.01) |
| North America | **ULUS10391** | Freedom Unite | Playable (v1.01): its own symbol map, overlay map and program (`mhfu-us`) |
| Japan | **ULJM05500** | Portable 2nd G | Third region; base of `mhp2g-decomp` and FUComplete |
| Others | PSN, "PSP Essentials"/"The Best", Korea/Asia | — | `[VERIFY]` whether variants with other hashes exist |

Known hashes for 2nd G JP (from FUCTool): UMD MD5 `1f76ee9ccbd6d39158f06e6e5354a5bd`, PSN MD5 `cc39d070b2d2c44c9ac8187e00b75dc4`.

**Evidence that per-region maps are required**: public CWCheat codes use different addresses per region (e.g. the same
"auto clear quest" cheat writes `0x01206628` on US and `0x012064E8` on EU). The binaries differ, so each region has its own
`symbols.toml`.

## 3. Verified facts of the reference EU dump (2026-10-16)

ISO:
- 886,702,080 bytes · SHA-1 `631e3d1651550617ed4b83351d874e6c06bd56b0` · MD5 `0449187d420ce68db2bf4d2886d3aa99`
  (not found by a web search; cross-check with Redump `[VERIFY]`).
- `PARAM.SFO`: `DISC_ID=ULES01213`, `DISC_VERSION=1.01`, `CATEGORY=UG` (UMD game), `PSP_SYSTEM_VER=5.03`, `REGION=32768`,
  `PARENTAL_LEVEL=5`.
- `EBOOT.BIN`: 1,910,384 bytes, `~PSP` header (encrypted) · SHA-1 `e7ad91caa8afe7840c61a39c90d491e3779c35c7`.
- `BOOT.BIN`: 1,910,044 bytes, **all zeros**.

Decrypted (prepared) executable. Players get it from the installer (`07-launcher-ux.md` §3b), which reads the encrypted
`EBOOT.BIN` above, checks its SHA-1 and writes the result to the per-user data folder; developers may also keep one in
`user_data/exe/`. Both are checked against these identification values:
- SHA-1 `04b60ee797bd628faa0525ebd52186fa05967963`, 1,910,384 bytes.
- ELF `EXEC`, MIPS, entry point `0x08906934`, **no relocations** (fixed-address executable).
- `.text` at `0x08804000`, size `0x109E38` (~1.04 MB, 272,270 instruction words). `.data` ~770 KB. Import stubs in `.sceStub.text`.
- Module name inside the ELF: `MonsterHunterPortable2ndG` — the EU build keeps the Japanese module name, so the codebases are
  very close (good for mhp2g-decomp's names and for cross-region symbol mapping).
- PSPRecomp analyzer: **4,028 functions**, **1,738 indirect call sites**, **272 imports in 34 libraries** (`05-runtime-hle.md` §1).
- `mhp2g-decomp` uses VRAM `0x08804000` for 2nd G's EBOOT as well, and Metrowerks (`mwccgap`) as the compiler — FU was most likely
  built with Metrowerks CodeWarrior for PSP, useful for recognizing compiler idioms.

## 4. Disc layout and code layout

- `PSP_GAME/PARAM.SFO` → `DISC_ID` identifies the region.
- `PSP_GAME/SYSDIR/EBOOT.BIN` (encrypted) and `BOOT.BIN` (zeros).
- `PSP_GAME/SYSDIR/UPDATE/` → Sony firmware updater (`EBOOT.BIN`, `DATA.BIN`, `PARAM.SFO`). **Ignored.**
- `PSP_GAME/ICON0.PNG`, `ICON1.PMF`, `PIC0.PNG`, `PIC1.PNG`, `SND0.AT3` → XMB artwork.
- `UMD_DATA.BIN` (48 bytes) at the root.
- `PSP_GAME/USRDIR/DATA.BIN` (772,732,928 bytes) → the **only** game data file: models (`.pac`), textures (`.tmh`), quests, text
  tables… **and the overlays**. No `.prx` modules on the disc.
- **298 code overlays** (12 task, 2 sub, 17 monster, 267 stage) stored in `DATA.BIN` and loaded at runtime into four contiguous
  regions. They contain ~81 % of the game's code. Extracted and validated on 2026-10-16; details in `04-recompiler.md` §4.
  Identification manifest: `games/mhfu/regions/ULES01213/overlays.toml`.
- In 2nd G many files inside `DATA.BIN` are **scrambled with the game's own cipher** (not Sony DRM); mhtools (`codestation/mhtools`,
  GPL-3) documents which indices and how. This is not our concern at runtime: the recompiled game decodes its own data with its own
  code, exactly as on the PSP. It matters only for overlay extraction on the developer machine (overlays are scrambled like every other file; verified).

## 5. Multi-region symbol map

`games/mhfu/regions/<ID>/symbols.toml`:
```toml
# canonical_name = address in this region
[functions]
mh_main             = 0x08906934   # entry point (EU)
mh_overlay_prepare  = 0x0890DD9C   # overlay-manager hook point (EU)
mh_camera_update    = 0x088xxxxx

[data]
mh_player_state     = 0x09xxxxxx
```
The EU map (`regions/ULES01213/symbols.toml`) currently holds the entry point, the overlay loader and the language screen's
scene counter (`04-recompiler.md` §4.4).
The US map (`regions/ULUS10391/`) holds the entry point, the overlay loader, the overlay manifest and the same counter; the
JP map is pending until that dump is available.

`mhf_recomp` turns every `[data]` entry into a `MHF_SYMBOL_<name>` macro in a generated header (`<program>_symbols.hpp`,
git-ignored like the generated code) that the host includes. That is how host code reads game state — the language
screen's scene counter, for example — by canonical name: a region's map decides whether the name exists, and an address
is never written by hand in the host. A missing name simply disables the behaviour that needs it in that region (§8).

**US release (ULUS10391, measured 2026-10-26).** Up to the overlay manager the code sits where it does in EU; from there on
everything, code and data, is 0x120 bytes higher, and the overlay groups load 0x100 higher. The 298 overlays carry the same names
and `DATA.BIN` ids as in EU (only `demo_task` differs in size). Recompiled it gives the same 1,042 generated files and the
same 272 system imports as EU, and it runs: language menu (the disc offers the same five languages), ESRB notice, intro movie,
character creation and the village. Its save-data key is passed by the game itself, so nothing about it is stored here.

Flow:
1. Find and name the function in EU (main development) with Ghidra.
2. Find its counterpart in US/JP with Ghidra Version Tracking / BinDiff / byte signatures.
3. Add it to each map. A `requires_game_data` test checks that every name used by patches exists in all supported regions.

**Name source**: `tclamb/mhp2g-decomp` (CC0, matching decompilation of 2nd G; last commit 2026-10-08) is the best source of
function and structure names (headers for camera, player, enemies, lobby network, quests, file system, GE…). CC0 lets us reuse
names and structures. **Any reconstructed C of Capcom functions in that repo is never copied into ours** (it would be the same art.
99.b transformation). Names, types and knowledge only.

## 6. Community tools and projects (reference, not dependencies)

| Project | License | What it is | Use for us |
|---|---|---|---|
| `tclamb/mhp2g-decomp` | CC0 | Matching decompilation of 2nd G (splat + Metrowerks) | Names/structures, overlay list |
| `SiD3W4y/mhfu-re` | (no visible license) | FU RE with an instrumented PPSSPP fork and traces | Instrumentation ideas |
| `IncognitoMan/mhff` (fork of `svanheulen/mhff`) | GPL-3.0 | Python scripts for MH formats (PSP and 3DS), Blender add-ons | Understand formats (overlay extraction, mods) |
| `codestation/mhtools` | GPL-3.0 | String tables, TMH, PAK, DATA.BIN scrambling, saves | Same; note it includes save cryptography |
| FUComplete (`FUComplete/Patch`, `FUCTool`) | — | Patch for 2nd G: English translation, built-in events, `nativePSP` file replacer | Shows what players want; incompatible with our legal model (patches the ISO) |
| FreedomUniteColorRandomizer | — | Recolors monsters (textures) | Mod idea |
| mfhu-hd-retexture | — | HD textures for PPSSPP | Never link (derived art) |

## 7. The game's network traffic (for `08-multiplayer.md`)

Observed by the PPSSPP community:
- Lobby/room: **UDP 10000**.
- In a quest: **UDP 20001–20004** (one port per player).
- The ad-hoc server (adhocctl) uses **TCP 27312**; the "postoffice" relay **TCP 27313**.
- Measured 2026-10-24 in the Gathering Hall (`05-runtime-hle.md` section 5i): the game creates one PDP socket on port 10000 and, once a
  quest is being set up, one on 20001; every datagram is a broadcast to the group; a hunter's position goes out every 40 vblanks
  (667 ms), the other two kinds of datagram every 16 vblanks (267 ms); it joins its group through the network configuration dialog
  (the group is named after the game, e.g. `MHP2Q000` on the servers) and does not use GameMode or Matching. Servers group the EU, US and JP
  releases of the game together (`ULES01213`, `ULUS10391`, `ULJM05500`).
- The game tolerates latency well; PPSSPP relays cite it as a title that works.
- Known original-game behavior: **small monsters are not synchronized** between players (not our bug).

## 8. Languages

The EU version is multilingual (EN/FR/DE/ES/IT). It reads the PSP's system language with
`sceUtilityGetSystemParamInt(8)` and then shows its own language screen before the title, on every start.

**Why the port answers that screen.** The screen is not a setting the game remembers: it does not save the answer, and
the game reads the system language only once, at start-up, to preselect a row. So a player who already told MHForever
which language they want (the launcher's *Language*, `--language`) would be asked the same question again at every
launch, with no way to say "don't ask". Answering the screen is how the port honours that choice. The clean
alternatives were tried or ruled out first (below): the screen is drawn by the start-up state machinery, not by a
function of its own, so there is no call to skip or hook; making `sceUtilityGetSystemParamInt(8)` fail does not remove
it. Answering it is therefore a deliberate, contained shortcut, not a guess at the game's flow.

**What is asked of that shortcut.** It must only act when a language was chosen (`--language`, else the boot behaves
exactly like the original), it must not change the language the game ends up with (it confirms the row the runtime's
answer already focused), and it must fail safe: the trigger is a read of a scene counter, so if a future exec has other
values or the read misses, no press is sent and the player sees the original screen. The counter is a data symbol of the
region, named in its `symbols.toml` and generated for the host; nothing like it is hardcoded in the runtime or in the
host.

**What the runtime does today.** `RuntimeOptions::system_language` (`0`, `1`, `2`, `3`, `4`, `5` for JP, EN, FR, ES, DE, IT;
English by default) is what `sceUtilityGetSystemParamInt(8)` answers, and `games/mhfu --language en|fr|de|es|it|jp` sets it.
The game reads it and opens its language screen with that language already focused, which saves the player from going
through the list. It still waits for a confirmation.

**What the game does with the language.** It reads the system language exactly once, at start-up, and keeps it only to
preselect a row of the screen: the answer is not saved and the game never asks the system again. Internally it uses its
own numbering, so the five PSP values are mapped on the way in (for example, French becomes the game's third language).

**The screen is not driven by that call.** It is drawn by the start-up state machinery: it has no timer and waits for
input indefinitely, no function of its own runs for it, and none of the overlay routines is involved. Hooking the
routines that decorate the start-up, and the demo overlay tasks, was tried and none of them fired while the screen is up.
The idea that a failing `sceUtilityGetSystemParamInt(8)` would skip the screen was disproved by experiment: the game
still shows it, simply with English focused.

**How the port skips it.** The screen does not consult the system and its state blocks never surfaced to afford a hook,
so the port answers it instead of editing the flow. When `--language` is given, the game focuses that very language, and
`games/mhfu` presses the game's own confirm as soon as the screen is up. The press is tied to the screen through the
boot's scene counter, a data symbol named in the region's `symbols.toml` and generated into a header the host includes:
the counter steps through the start-up screens and holds one value while the language screen waits, so the press follows
a slow or fast boot instead of a fixed delay. A region whose symbol map does not name that counter only preselects the
language and presses nothing: the same address means something else in another build, so a map without it is respected
instead of guessing.

The cross is pulsed, press and release for ~150 ms each, while the counter stays at that value and for at most 6 s: the
counter moves with the screen's fade-in, which ignores input, and the screen reads a fresh press, so a cross merely held
from the start never counts. The pad state is refreshed on every poll of the sequence, including the released half;
leaving it untouched keeps the cross latched and the screen never sees a new press (this was the first failing attempt).
The refresh stops as soon as the screen is answered: the frontend sets the player's own input on the same poll just
before `games/mhfu`'s callback runs, so writing the pad afterwards would keep clearing it and the controls would stop
working (this was the second one). The screen is on display only until it starts listening, roughly a second and a half,
and is never shown to the player as a choice. Without `--language` the boot behaves exactly like the original. Verified
end to end on both the European and the North American program with `--language es` and `it` (title screen and later boot
screens come out in the picked language) and without it (the screen appears as always). The launcher exposes the same
choice per copy of the game (*Ask* keeps the original screen, see `07-launcher-ux.md` §3b).

**Ruled out**, so nobody repeats the work: forcing a language variable that the game already writes before the screen,
the `demo_task`/`demo_sub` overlays, a saved game (`MHP2NDG.BIN`), `PARAM.SFO`, and a timer that advances the screen.
The measurements behind all of this — addresses, traces and the candidates tried — stay out of the repository, in the
local `research/` notes (`research/README.md`).

## 9. Desired enhancements (phase 7, all toggleable and independent)

| Enhancement | Estimated difficulty | Notes |
|---|---|---|
| Internal resolution ×N | Low | Renderer. Done: `--scale`, launcher setting |
| Texture filtering / MSAA | Low | Renderer. Bilinear option done (`--smooth-textures`); MSAA not |
| Widescreen 16:9 / 16:10 (Deck) | Medium | Projection + HUD; HUD may stretch |
| Right-stick camera | Medium | Level 1 (stick → D-pad, `07`) done. Level 2: hook the camera function |
| 60 fps | Done (0.2.0, experimental) | Renderer-side frame interpolation, 60 to 180 fps (`--fps`, launcher setting, `05` §6). Game logic stays at 30 fps, so timing and multiplayer sync are untouched; a real 60 fps logic would still need a timer study |
| Button icons per controller | Medium | Replace button textures at runtime with our own art |
| Own pause/system menu | Low | |
| Cloud-synced saves | Low | Configurable folder |

## 10. Specific risks

- System fonts: FU imports `sceLibFont` → free replacement fonts with compatible metrics (no PGF).
- PSMF intro videos (a real HLE of `sceMpeg`, implemented above in `05`).
- Imprecise SAS reverb (known in PPSSPP for FU).
- Cross-region multiplayer: regions probably cannot play together in the original game `[VERIFY]`; the lobby must group by region.
- Overlays: extraction and runtime routing are mandatory before any gameplay works.
