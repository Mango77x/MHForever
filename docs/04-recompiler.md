# 04 — Allegrex → C++ static recompiler

## 1. Evaluation of existing tools (done 2026-10-16)

| Project | License | Language | What we measured | Verdict |
|---|---|---|---|---|
| **PSPRecomp** (jessicanataliagta) | MIT | C++20, CMake | Framework core is small (~5k lines: decoder, ELF/PRX loader, guest memory, analysis, code generator). **Builds and passes its tests on Linux** (clang). Its decoder handles all but **55 of 272,270 words** of FU EU's main `.text`, with broad VFPU coverage. Its analyzer finds **4,028 functions** and **272 imports** in FU EU. **But** all of its HLE, GE renderer and audio live inside the GTA VCS profile (a 530 KB monolithic file, DX12/Windows only), and that profile **commits generated game code** to the repo | **Use its generator/decoder/loader as our base.** Do not reuse its runtime |
| **Ace Combat X Recompilation** (sal063) | GPL-2.0 | Python (recompiler) + C (runtime) | Mostly generic runtime: ~12k lines of HLE (threads with ucontext/fibers, I/O, savedata, SAS, ATRAC3+, MPEG, dialogs, fonts), ~8k lines GE→Vulkan, SDL3 host, MIPS/VFPU interpreter for traps and cold paths, boot-trace golden test. Does not commit generated code. Windows-focused; bundles Sony PGF fonts (we won't) | **Design reference for our runtime.** Reuse code only if D-11 = GPL-2.0-only |
| **N64Recomp** | MIT | C++ | Mature (other MIPS CPU) | Technique reference: delay slots, jump tables, `LOOKUP_FUNC`, relocations, overlays, single-file patch mode |

**Recommendation**: PSPRecomp's generator + our own generic runtime modeled on ACX recomp's, with N64Recomp's
overlay technique. PSPRecomp's code is vendored into `third_party/` or forked into `platforms/psp/tools/` with its MIT notice.

## 2. Inputs

1. Decrypted ELF (developer-only, from own copy).
2. Overlay binaries extracted from `DATA.BIN` (developer-only; §4).
3. `recomp.toml` per region: paths, functions to skip/replace, instruction patches, jump-table hints.
4. `symbols.toml` (optional but improves output and enables hooks).
5. `overlays.toml`: overlay list, load address, file location inside `DATA.BIN`.
6. Platform NID database.

## 3. Analysis

1. **Load** sections (`.text`, `.data`, `.rodata`, `.bss`), apply **relocations** if any (FU's EBOOT is a fixed `EXEC` ELF with
   no relocations to apply), and use the same base address as the PSP loader so data pointers match.
2. **Resolve imports** by NID → HLE symbols.
3. **Discover functions**: from the entry point, exports, `jal` targets, pointers in `.data`/`.rodata` that point into `.text`
   (vtables, callback tables), typical prologues. Merge with a Ghidra export if needed.
4. **Jump tables**: the `lui/addiu/sll/addu/lw/jr` pattern → `switch`, bounds taken from the preceding comparison.
5. **Data in code / code in data**: mark doubtful regions for the fallback interpreter.
6. Coverage report: % of `.text` covered by known functions.

## 4. Overlays (critical for FU)

### 4.1 What we measured (EU, 2026-10-16)

- The ELF declares **298 overlays** as zero-size sections. Their bytes live in `DATA.BIN` and are loaded at runtime.
- **The overlays hold ~81 % of the game's code**: 1,173,963 instruction words in overlays vs 272,270 in the main `.text`
  (~4.5 MB vs ~1 MB). PSPRecomp's decoder handles **all** overlay instructions (0 unsupported).
- Four contiguous regions, nested in this order (load address → maximum end including BSS):

  | Region | Load address | Overlays | Largest extent | Contents |
  |---|---|---|---|---|
  | task | `0x09A5F200` | 12 (`*_task.ovl`: demo, edit, cont, option, gallery, download, install, lobby, game, show_select, arcade, movie) | `0x1B9E00` → ends at `0x09C19000` | Top-level game modes |
  | sub | `0x09C19000` | 2 (`demo_sub.ovl`, `game_sub.ovl`) | `0x101180` → ends at `0x09D1A180` | Mode subsystems |
  | em | `0x09D1A180` | 17 (`emNN.ovl`) | `0x48E80` → ends at `0x09D63000` | Monster logic |
  | stage | `0x09D63000` | 267 (`stage000`–`stage266.ovl`) | `0x2400` | Per-stage logic |

- Call graph (`jal` targets, 81,006 total): 26,403 into the main EBOOT, 37,353 within the same overlay, 17,250 **into other
  overlay regions** — mostly from em/stage/sub overlays into the task region (i.e. into `game_task.ovl`) and the sub region.
  So overlays depend on the overlays loaded "above" them: stage/em → sub → task → EBOOT.

### 4.2 Container and header format

- `DATA.BIN` is a block container (2048-byte blocks) with a scrambled header: first-block index per file + a sentinel equal to the
  total block count, then `(file_id, size)` pairs for files that do not end on a block boundary. EU: **6,657 files**, 813 pairs,
  header = 17 blocks. (JP 2nd G has 6,603 files per mhp2g-decomp.)
- Every file is scrambled with the game's own scheme: per-byte substitution table (256 bytes, inside the executable) + XOR with a
  keystream seeded by the file's first block. This is the game's data obfuscation, not Sony DRM.
- Overlays start with a 64-byte Metrowerks header: magic `MWo3`, overlay id, load address, text size, data size, BSS size,
  static-initializer range, name (32 bytes). File size = 64 + text + data. Code begins after 64 bytes of padding (offset 0x80).
- Overlay file ids in EU: 63–74 (task), 75–76 (sub), 5542–5808 (stage), 6095–6111 (em). JP uses 43–54, 55–56, 5490+, 6043+
  (per mhp2g-decomp), so ids differ by region and the extractor discovers overlays by scanning for the `MWo3` magic.

### 4.3 Extraction (developer machine only)

`games/mhfu/tools/extract_overlays.cpp` (`mhfu_extract_overlays --iso <iso> --exe <decrypted elf>`):
1. Reads `DATA.BIN` straight from the ISO (`platforms/psp/tools/iso9660`).
2. **Embeds no game data**: finds the substitution table inside the developer's own executable (every 256-byte window that is a
   permutation of 0–255; EU has 4 candidates) and keeps the one that decodes a consistent header.
3. Writes every overlay to `user_data/overlays/<DISC_ID>/` plus `overlays.toml` (name, file id, load address, sizes, SHA-256).
   That manifest is identification data and is versioned in `games/mhfu/regions/<DISC_ID>/overlays.toml`.
4. EU run: 298 overlays, 6.8 MB, ~7 s.

### 4.4 Recompilation and runtime design

**Loader, as measured in the EU executable (2026-10-16).** Canonical names and addresses are in
`games/mhfu/regions/ULES01213/symbols.toml`.

- The game keeps one slot per overlay *group*: 0 eboot, 1 task, 2 sub, 3 em, 4 stage, 5 debug. A table of six region base
  addresses (`mh_overlay_region_bases`) and a `u16[6]` array of active DATA.BIN file ids inside the `System` object
  (`system_active_overlays`, initialized to `0xFFFF`) describe the state. This matches `activeOverlays` in mhp2g-decomp.
- Loading is two-step. `mh_overlay_request(system, file_id, group)` starts an asynchronous read of the whole overlay file
  (header included) to the group's base address. `mh_overlay_finish(system, file_id, group)` completes it, calls
  `mh_overlay_prepare(image, size)` and then records `file_id` as active. `mh_overlay_is_active` compares the slot.
- `mh_overlay_prepare` is the single choke point: every overlay goes through it, from one call site. It does cache maintenance,
  zeroes `.bss` right after the image, and runs the overlay's static-initializer table (`static_init_start..end` from the
  header) through `mh_static_init_run`, which the executable also uses for its own constructors.
- Request/finish pairs are called from 18 sites in the executable and from three task overlays (`game_task`, `lobby_task`,
  `movie_task`). The em and stage overlays never load other overlays.
- The image is mapped verbatim: guest address = region base + file offset, header included. All 298 headers carry a unique
  `overlay_id` (1–298). 271 of them have a non-empty static-initializer table (one entry in most stage overlays).
- Cross-reference: the JP 2nd G functions `func_eboot_0888FE4C` / `func_eboot_0888FF00` / `func_eboot_0888FEE8` in
  mhp2g-decomp are the counterparts of request / finish / is-loading, shifted by the same constant, which suggests the US build
  can be mapped with byte signatures.

**Design.**

- **Recompilation**: every overlay is recompiled at its load address into its own namespace (`ovl_stage042::f_09D63A10`).
  Overlays sharing an address range coexist in the binary.
- **Runtime overlay manager**: hooks the entry of `mh_overlay_prepare`. The hook reads the 64-byte header at `image`,
  identifies the overlay by `overlay_id` (and checks the name), and marks it active for the region that contains its load
  address. It then runs the recompiled original, so the static initializers are dispatched with the new overlay already active.
  The bytes stay in guest memory, because overlay data is read normally.
- **Dispatch by region**: an indirect call or cross-region `jal` into a region's range resolves to the active overlay of that
  region. Each region has exactly one slot and calls only go downward (stage/em → sub → task → executable), so there is no
  combination table to maintain: any mix of active overlays is valid.
- **Implemented (2026-10-16)**: `mhf_recomp --overlays overlays.toml` analyzes the executable and every overlay together
  (two rounds: calls from overlays seed the executable and vice versa; calls into a shared region become weak seeds of each
  overlay that covers the address). Each overlay's whole image after the header is treated as code and data (static
  constructors follow the data section) and its static-initializer table seeds entries. Each overlay is emitted in its own
  namespace with its address range and id; `<name>_programs()` lists all modules. Measured on EU: executable 3,306
  functions (96.9% of `.text` reached); overlays 16,177 functions.
- **Validation**: in debug builds, hash the loaded image on activation and compare with the manifest; on mismatch fall back to
  the interpreter and log it.
- N64Recomp handles relocatable overlays similarly (`LOOKUP_FUNC`, section index + `RELOC_HI16/LO16`). FU's overlays are
  loaded at fixed addresses and the loader applies no relocations, so none are needed.

## 5. Emission

**Implemented (2026-10-16)**: `platforms/psp/recomp` (`mhf_recomp`). Function discovery (`analysis.cpp`): seeds from the entry
point, `jal` targets, `lui`+`addiu/ori` constants that point into code (scanned in address order, delay slots included) and
data words that point into code. Code pointers that land inside a known body are labels, not functions. `j` to another
function is a tail call. Jump tables are decoded from the `lui/addiu/addu/lw/jr` pattern bounded by `sltiu`; for computed
jumps it cannot decode, runs of three or more data words pointing into the function's address range are taken as its tables.
The fallback for any remaining computed jump is a `switch` over every block of the function, then an indirect transfer.
Non-local returns (`longjmp`) are handled at each call site: if the link register after the call is not the address after
the call, the frame resumes at the matching call continuation or returns to its own caller (`homebrew/cputest`).
Instruction semantics come from PSPRecomp's emitter plus our additions (`madd/maddu/msub/msubu`, `vsgn`, `vsocp`, `break`).

Original design notes:

- One C++ function per guest function: `void f_08804A20(Context* ctx, uint8_t* mem)`; canonical name if present in `symbols.toml`.
- **Literal** per-instruction translation (as N64Recomp: `addiu $r4,$r4,0x20` → `ctx->r4 = ctx->r4 + 0x20;`).
- **Delay slots**: duplicate the slot instruction into each path; *likely* variants only on the taken path.
- `jal` → direct C call. `j`/`b` leaving the function → tail call. `jr ra` → `return`.
- Unresolved `jalr`/`jr` → `ctx->lookup(addr)(ctx, mem)` with a generated table (overlay-aware), controlled abort + log if missing.
- Memory accesses → `MEM_R32(addr)` etc. over a single guest memory block (with alias mask).
- `syscall` → `hle_dispatch(ctx, id)`.
- FPU and VFPU → inline functions in `platforms/psp/runtime/cpu/vfpu.h`.
- `halt`/waits → yield to the scheduler.
- Output split into many files (e.g. 500 functions per `.cpp`) for parallel builds.

## 6. Hooks and patches

- **Replacement**: a guest function replaced by a native one registered by the profile (`register_function("mh_xxx", impl)`).
  Used for enhancements (camera, framerate) and fast paths (memcpy, memset, hot VFPU routines).
- **Hooks**: call native code before/after a guest function without replacing it.
- **Instruction patch**: change a single instruction (last resort).
- Everything is declared by **canonical name** and resolved per region. Never addresses in code.
- N64Recomp technique: compile patched functions into a library linked **before** the original output, so the linker prefers the
  patch. Useful to iterate without regenerating everything.

## 7. Fallback interpreter

Needed for: code generated at runtime (rare on PSP), jumps to undiscovered addresses, doubtful regions, overlays whose bytes do not
match. ACX recomp has one (`runtime/cpu`). Every use is logged; in a stable build the goal is **zero** uses on the normal path.

**Implemented (2026-10-16)**: `platforms/psp/runtime/src/interpreter.cpp` (integer, FPU, loads/stores, branches) and
`interpreter_vfpu.cpp` (every VFPU form the emitter lowers, mirrored case by case). `Runtime::call` falls back to it for
any address in user RAM without a recompiled function; calls out of interpreted code go native again when a recompiled
function exists. `MHF_INTERPRET_ONLY=1` forces it for everything except HLE stubs; CTest runs every homebrew test both
ways, which is the cheap form of the differential testing below.

## 8. Verification

1. **Golden boot trace** (ACX technique): run until the first N HLE calls and compare with a `golden` file. In CI with homebrew;
   with the game only locally (`requires_game_data`), and the golden file is **not committed** if it contains game data.
2. **Differential against the interpreter**: run the same function recompiled and interpreted from the same state and compare
   registers/memory. Excellent for catching VFPU and delay-slot bugs.
3. **Per-instruction unit tests** with our own homebrew (`homebrew/cputest`, `homebrew/vfputest`).
4. Visual comparison with PPSSPP (locally by the developer, never publishing screenshots).

## 9. Determinism

For multiplayer, two clients with the same region and data must behave like two PSPs. The game already syncs what it needs over the
network (it is not lockstep), so full determinism is not required, but floating-point semantics must match across Windows and
Linux: no `-ffast-math`, `-ffp-contract=off`, no x87, identical VFPU results; cross-platform tests.
