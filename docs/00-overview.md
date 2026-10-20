# 00 — Overview and scope

## 1. What MHForever is

A **single platform** (one launcher) from which players run classic games of the series **natively on PC**, with any
controller and with friends over the internet — in the spirit of PokeMMO: the program ships no game assets and every player
provides their own copy.

- **Native** means the game's code is translated *ahead of time* (static recompilation) to C++ and compiled for x86-64/ARM64.
  There is no MIPS interpreter or JIT on the hot path.
- **Legal by design**: the source repository only contains our own code. Players provide their own ISO; all game data is read
  from it. See `CONTRIBUTING.md`.
- **Modular**: each game is an independent module built on a per-console runtime (PSP first; later PS2/3DS/Wii) and a
  shared core (launcher, input, network, rendering).

**First module:** Freedom Unite (PSP), supporting all three regions (EU, US, JP/2nd G). Development starts on EU (ULES01213).

## 2. What it is not

- Not a general PSP emulator (it does not load arbitrary ISOs; only titles with a module).
- Not a remake or clean-room reimplementation (a possible long-term evolution, see §5).
- It does not include or download game assets.

## 3. Why static recompilation (and not manual decompilation)

| Approach | Example | Cost | Result |
|---|---|---|---|
| Matching decompilation | Ship of Harkinian (Ocarina of Time), SM64 | Years per game | Readable source, best for mods |
| **Static recompilation** | Zelda64Recomp (N64Recomp), Ace Combat X (PSP), GTA VCS (PSPRecomp) | Weeks-months per game | Native executable; code is a literal translation |
| Emulation | PPSSPP | Already exists | Not what we want |

Recompilation is the only approach that scales to "several games". The existing 2nd G decompilation (`tclamb/mhp2g-decomp`)
is used as a **knowledge source** (names, structures), not as the base of the port.

## 4. Glossary

- **Allegrex**: the PSP's MIPS32 CPU. **VFPU**: its vector coprocessor (COP2).
- **GE**: Graphics Engine, the PSP GPU (32-bit command lists).
- **HLE**: High-Level Emulation; reimplementing OS functions (sceKernel*, sceIo*…) on the host instead of emulating hardware.
- **NID**: 32-bit identifier of each function imported from a PSP system library.
- **EBOOT.BIN / BOOT.BIN**: main executable in `PSP_GAME/SYSDIR/`. EBOOT is usually encrypted; BOOT.BIN is an unencrypted copy
  in some early games and zero-filled in later ones (zero-filled in FU EU).
- **Overlay**: a block of code+data loaded at runtime into a fixed address region, replacing another overlay. FU uses ~300.
- **Profile / game module**: everything title-specific (`games/<id>/`).
- **Region**: a market variant of the game with its own disc ID (ULES01213, ULUS10391, ULJM05500…).
- **Ad-hoc**: the PSP's local wireless multiplayer, which we tunnel over the internet.

## 5. Phased vision (summary; details in `10-roadmap.md`)

0. Foundations: repo, CI, legal, name, test homebrew.
1. Minimal PSP runtime on our own homebrew (window, controller, audio, GE→OpenGL 3.3; Vulkan optional later).
2. Freedom Unite boots to the main menu (EU first).
3. Village and first quests playable, saving works.
4. Launcher and packaging.
5. Multiplayer (ad-hoc over the internet).
6. US and JP regions.
7. Optional enhancements (widescreen, high resolution, 60 fps, analog camera).
8. Next game (Portable 3rd or Freedom/Freedom 2), reusing the PSP runtime. Long term: other consoles (PS2 when PS2Recomp
   matures, 3DS) and progressive reimplementation of modules.
