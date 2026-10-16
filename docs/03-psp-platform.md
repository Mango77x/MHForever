# 03 — PSP platform reference

Summary for whoever implements the recompiler and runtime. Primary sources: PPSSPP docs, uPSPD/PSP Dev Wiki,
pspdev/vfpu-docs and Copetti. Facts written from general knowledge are tagged `[VERIFY]`.

## 1. CPU — Allegrex

- 32-bit MIPS32, little-endian, R4000-based, **222 MHz** by default and **333 MHz** with later firmware.
- Extra instructions over MIPS II: `ext`, `ins`, `wsbw`/`wsbh`, `seb`, `seh`, `rotr`/`rotrv`, `bitrev`, `clz`, `clo`, `max`/`min`,
  `madd(u)`/`msub(u)`; Allegrex-specific: `halt`, `mfic`, `mtic`.
- **Missing**: 64-bit instructions, `ll`/`sc`, `ldc1`/`sdc1`, traps (`t*`), TLB, `bltzal`/`bgezal(l)`.
- **Delay slots** on branches and jumps as on every MIPS; "likely" variants exist.
- **Coprocessors**: COP0 (control; no TLB), **COP1 FPU** single precision only, **COP2 VFPU**.
- **No MMU/TLB**: memory protection only (user/kernel). Physical addresses with aliases (high bits for cached/uncached/kernel).
  The runtime must mask aliases. `[VERIFY exact mask]`
- **Explicit cache control**: the GPU does not read the CPU cache; games call `sceKernelDcacheWritebackRange` before handing data
  to the GE. In a native runtime this is mostly a no-op, but **ordering** with the GE matters.
- 16 KiB **scratchpad** on the main CPU. `[VERIFY address 0x00010000]`
- **Media Engine**: a second Allegrex core without VFPU for audio/video decoding. Not programmable by games; its functions are
  reimplemented as HLE (MPEG/ATRAC).

## 2. VFPU

- 128 float32 registers arranged as **8 4×4 matrices**, addressed as scalars, rows, columns, 2/3/4 vectors or 2×2/3×3/4×4
  matrices (with transpose).
- **Prefixes** (`vpfxs`, `vpfxt`, `vpfxd`) modify operands (swizzle, negation, constants, saturation, write mask).
- Control registers (e.g. `VFPU_CC` for comparisons). Hazard: an instruction is needed between `vcmp` and `mfvc` to read the
  correct `CC`.
- Some matrix operations require that input and output **do not overlap**.
- Some operations deviate from IEEE-754 (sine, cosine, approximate reciprocals). Numeric fidelity matters for multiplayer
  consistency. Semantics reference: **pspdev/vfpu-docs** (formal YAML + tests on real hardware).
- Plan: every VFPU instruction becomes an inline C++ function, SSE where possible plus a scalar reference version; tests against
  real-hardware results (from our own homebrew on a real PSP if available) or against vfpu-docs.

## 3. Memory `[VERIFY all]`

| Region | Address | Size |
|---|---|---|
| Scratchpad | 0x00010000 | 16 KiB |
| VRAM (GE eDRAM) | 0x04000000 | 2 MiB (mirrored depending on model) |
| Kernel RAM | 0x08000000 | 8 MiB |
| User RAM | 0x08800000 | 24 MiB (PSP-1000). Models 2000+ have 64 MiB total; 2009 games normally still use 24 MiB unless they ask |

Aliases with bit 0x40000000 (uncached) and 0x80000000 (kernel) point to the same physical memory.
FU EU's main `.text` starts at 0x08804000 and its overlay regions sit between 0x09A5F200 and ~0x09D66000 (measured).

## 4. GE (Graphics Engine)

Main source: PPSSPP's documentation "The GE".

- **Command lists** in RAM, 32-bit words: 8-bit opcode + 24-bit parameter. Floats (matrices, lights) use **float24** (the low 8
  mantissa bits are dropped).
- Flow control: `JUMP`, `BJUMP` (bounding-box jump), `CALL`/`RET`, `END`, `FINISH`, `SIGNAL`, `OFFSET_ADDR`, `ORIGIN_ADDR`.
- Games enqueue lists with `sceGeListEnQueue`/`sceGeListSync`/`sceGeDrawSync`; there are `SIGNAL`/`FINISH` callbacks. Some games
  write or load raw lists from disk.
- **Fixed-function T&L**: world/view/projection matrices, up to 4 lights, skinning with weights, morphing. **Through mode**: vertices
  already in screen coordinates, no transform or clipping.
- **Vertex formats** with fixed component order: color (565, 5551, 4444, 8888); normals/positions/weights/UVs as 8-bit, 16-bit or
  float. 8/16-bit UVs carry scale/offset; an 8-bit UV is divided by 128 (range 0–2).
- **Primitives**: points, lines, strips, triangles, strips, fans, **RECTANGLE** (2-point sprite with rotated UVs), hardware
  **Bézier and B-spline patches**.
- **Clipping**: near plane only (with odd behavior); the rest by *guardband*. Triangles reaching outside a virtual 4096×4096
  viewport are **discarded**, and some games rely on it.
- **Viewport** is a scale/offset (not a rectangle); the **scissor** is the only thing bounding the drawing area.
- **Framebuffers** in VRAM: RGBA5551, RGB565, RGBA4444, RGBA8888. A single 16-bit depth format (memory layout differs by color
  format). **Stencil is the color alpha channel** (1, 4 or 8 bits; none in 565).
- **VRAM aliasing**: a game may render in one format and read that memory as a texture in another (e.g. paletted) — the basis of
  many post-processing effects. The renderer's framebuffer manager must detect such reads.
- **Textures** from VRAM or RAM, swizzled or linear, paletted (4/8/16/32-bit CLUT) with **shift/mask** before lookup, DXT1/3/5.
  Single texture stage, simple combiners.
- **Blending**: standard factors plus 2×SRC, 2×DST, their negations and the `ABSDIFF` op. **Per-bit** color write mask.
- Block transfers (`TRXKICK`) between memory areas.
- The game passes the framebuffer to display with `sceDisplaySetFrameBuf`; native resolution is **480×272**.

## 5. Audio

- Simple PCM channels (`sceAudioChReserve`/`sceAudioOutputBlocking` and variants).
- **SAS** (`sceSasCore`): software voice mixer with VAG ADPCM, ADSR envelopes and **reverb**. PPSSPP admits its reverb is an
  approximation (PS1 reverb with guessed parameters); reverb issues are known in FU and Portable 3rd. Improvement opportunity,
  not a blocker.
- **ATRAC3/ATRAC3+** (`sceAtrac3plus*`): compressed music and effects, decoded by the Media Engine. Needs a decoder (FFmpeg has one).
- **PSMF/MPEG** (`sceMpeg`, `scePsmf`): intro videos. H.264 + ATRAC decoder (FFmpeg).

## 6. Operating system and HLE

- Games import firmware library functions by **NID** (32-bit). The import table (`.lib.stub`) points to `syscall`/`jr ra` stubs
  that the runtime binds to HLE implementations.
- NID names: "PSP PRX Libraries Documentation" project (XML), `psp-ghidra-scripts`, `prxtool`, pspsdk headers. We store only
  **names and signatures** (interoperability data) in `platforms/psp/data/`.
- Threads: the PSP kernel is priority-based and cooperative among same-priority user threads (switches happen on blocking calls or
  yields). The runtime needs a matching scheduler (`05-runtime-hle.md` §2).
- Modules loaded from disc (`.prx`) can be system ones (replaced by HLE) or the game's own (must be recompiled too). FU EU has
  none on disc; its extra code comes as overlays (`06-mhfu-module.md` §4).

## 7. Container formats

- **ISO**: ISO9660, 2048-byte sectors, no Joliet/UDF.
- `PSP_GAME/PARAM.SFO`: metadata (DISC_ID, title, version, region, parental level). Used to identify the region.
- `PSP_GAME/SYSDIR/EBOOT.BIN`: encrypted executable (`~PSP`). `BOOT.BIN`: unencrypted copy (may be zero-filled; it is in FU EU).
- `PSP_GAME/SYSDIR/UPDATE/`: Sony firmware updater. Ignore.
- `PSP_GAME/USRDIR/`: game data (in FU, only `DATA.BIN`).
- `PSP_GAME/ICON0.PNG`, `ICON1.PMF`, `PIC0.PNG`, `PIC1.PNG`, `SND0.AT3`: XMB artwork (publisher art: never on the project website;
  the launcher may show them **read from the player's copy**).
