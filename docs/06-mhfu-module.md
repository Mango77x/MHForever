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
| North America | **ULUS10391** | Freedom Unite | Second region |
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
The EU map (`regions/ULES01213/symbols.toml`) currently holds the entry point and the overlay loader (`04-recompiler.md` §4.4).
US and JP maps are pending until those dumps are available.

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

The EU version is probably multilingual (EN/FR/DE/ES/IT) and picks the language from the PSP system setting (`sceImpose`/
`sceUtility` system params). Expose it as a launcher setting. `[VERIFY]`

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
- PSMF intro videos: can be skipped at first.
- Imprecise SAS reverb (known in PPSSPP for FU).
- Cross-region multiplayer: regions probably cannot play together in the original game `[VERIFY]`; the lobby must group by region.
- Overlays: extraction and runtime routing are mandatory before any gameplay works.
