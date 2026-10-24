# 05 — PSP runtime and HLE

The runtime replaces the PSP firmware. It is implemented **library by library, as the game imports them**.

## 1. Measured imports of FU EU (ULES01213 1.01)

Measured on 2026-10-16 with PSPRecomp's analyzer on the decrypted EBOOT: **272 functions from 34 libraries**.
(Library names and counts are interoperability data; the full NID list lives in `research/` until the NID database exists.)

| Priority | Library | Functions | Purpose / notes |
|---|---|---|---|
| P0 | `ThreadManForUser` | 37 | Threads, semaphores, event flags, mutexes, mailboxes, alarms, delays, callbacks. Core; see §2 |
| P0 | `SysMemUserForUser` | 6 | Memory allocation, SDK version |
| P0 | `ModuleMgrForUser` | 7 | Module load/start (also relevant to overlays) |
| P0 | `IoFileMgrForUser` | 10 | Files: `disc0:`, `umd0:`, `ms0:` (saves), async reads |
| P0 | `sceDisplay` | 2 | Framebuffer, vblank — the 60 Hz master clock |
| P0 | `sceGe_user` | 9 | List enqueue/sync, signal callbacks → `platforms/psp/gpu` |
| P0 | `sceCtrl` | 3 | Controller → `core/input` |
| P0 | `UtilsForUser` | 8 | Cache ops, time, MT19937 |
| P0 | `Kernel_Library` | 2 | Interrupt enable/disable, etc. |
| P0 | `InterruptManager` | 3 | Interrupt handlers (vblank) |
| P0 | `LoadExecForUser` | 2 | Exit game, exit callback |
| P0 | `StdioForUser` | 3 | Stdout/stderr (debug output) |
| P0 | `sceSuspendForUser` | 3 | Power lock, volatile memory |
| P0 | `sceDmac` | 1 | DMA memcpy |
| P0 | `sceUmdUser` | 3 | UMD present/activate — trivial stubs |
| P1 | `sceAudio` | 7 | PCM output |
| P1 | `sceSasCore` | 14 | Voice mixer; see `03-psp-platform.md` §5 |
| P1 | `sceAtrac3plus` | 10 | Music; external decoder |
| P1 | `sceUtility` | 19 | Savedata dialog, message/OSK dialogs, net config, system params (language) → our own UI |
| P1 | `sceImpose` | 1 | System language/button assignment |
| P1 | `scePower` | 1 | CPU clock / battery |
| P1 | `sceRtc` | 1 | Real-time clock |
| P1 | `sceLibFont` | 9 | **The game uses system fonts.** We cannot ship Sony's PGF fonts: implement `sceLibFont` over free fonts (e.g. Noto Sans/Noto Sans JP) with metrics close enough. Risky area for text layout `[VERIFY which fonts/styles the game requests]` |
| P2 | `sceMpeg` | 24 | Intro/cutscene videos; can be skipped initially |
| P2 | `sceNetAdhoc` | 15 | Multiplayer data (PDP/PTP); `08-multiplayer.md` |
| P2 | `sceNetAdhocctl` | 8 | Multiplayer groups/rooms |
| P2 | `sceNetAdhocDiscover` | 5 | Ad-hoc discovery |
| P2 | `sceWlanDrv` | 2 | Wi-Fi switch state, MAC |
| P2 | `sceNet` | 4 | Network init |
| P3 | `sceHttp` | 28 | **Infrastructure mode**: downloading DLC/event quests from Capcom's long-gone servers `[VERIFY]` |
| P3 | `sceNetInet` | 16 | Sockets for infrastructure mode |
| P3 | `sceNetApctl` | 6 | Access point connection |
| P3 | `sceSsl` | 2 | HTTPS |
| P3 | `sceOpenPSID` | 1 | Console ID (used for multiplayer/save identity?) — return a stable per-install ID |

Infrastructure (P3): initially return "no connection". Later option: serve event quests the player already owns from local files
(a "local download server"), never hosting Capcom content ourselves.

Every unimplemented function must: log the NID and arguments the first time, return a reasonable error and not abort (except in
strict development mode).

## 2. Threads and scheduling

**Implemented (2026-10-16, `platforms/psp/runtime/src/kernel.cpp`)**: option 2, one host **fiber** per PSP thread
(Windows fibers / `ucontext`), the scheduler on the host thread's own fiber. Recompiled code keeps using the native stack of
the fiber, so no generated code has to know about threads. Semantics:
- strict priority (lower value first), FIFO among equal priorities; a thread runs until it blocks, yields, exits or a
  higher-priority thread becomes ready (checked after every HLE call that wakes threads or changes priorities); a preempted
  thread resumes first among its priority;
- timed waits use the host monotonic clock; remaining time is written back to the guest's timeout word;
- a thread whose entry function returns exits with `$v0` (dormant, restartable);
- new stacks are filled with `0xFF`, the top 0x100 bytes are reserved (`$k0`), start arguments are copied below it.

Implemented objects: threads (create/start/exit/terminate/delete/wait-end/sleep/wakeup/priority/refer), semaphores, event
flags (AND/OR, clear, multi-wait attribute), lightweight mutexes (work area in guest memory), minimal callbacks (created and
notified; not yet dispatched at CB waits). Covered by `homebrew/threadtest`.

Option 1 (one OS thread per guest thread with a global token) was the earlier recommendation; fibers were chosen because they
are deterministic, cheap and avoid OS scheduling jitter. The scheduler stays behind `Kernel` if this ever needs revisiting.

## 3. Time

- Virtual clock at 222 or 333 MHz per `scePowerSetClockFrequency`; `sceKernelGetSystemTimeLow` in real microseconds.
- Vblank at 59.94 Hz as master signal. The game flips at 30 fps (measured on the owner's disc); higher rates come from
  renderer-side frame interpolation (§6), not from the game logic (`06-mhfu-module.md` §9).

## 4. File system

**Implemented so far**: `IoFileMgrForUser` and `StdioForUser` over host folders (`ms0:`, `disc0:`/`umd0:`, `host0:`), with
path normalization that cannot escape the device root; asynchronous calls complete immediately and keep their result for
`sceIoWaitAsync`/`sceIoPollAsync`. Serving `disc0:` straight from the ISO is next.

- `disc0:/PSP_GAME/...` → read directly from the player's ISO through the ISO9660 reader.
- Sector reads (`umd0:` / LBA): use the LBA manifest built when indexing.
- `ms0:/PSP/SAVEDATA/<GAMEID><SLOT>/` → the user data folder (`07-launcher-ux.md` §7).
- PSP saves are encrypted by `sceUtility` with the game's own key; we read and write that format (§5d), so saves
  are interchangeable with PPSSPP.

## 5. Overlay manager

See `04-recompiler.md` §4. Hooks the game's overlay loader, tracks which overlay occupies each region, and routes indirect calls.

**Implemented**: `Runtime::add_overlay` / `activate_overlay` / lookup by region (`runtime.cpp`). Generated code calls
`find_hook("<canonical name>")` on entry to hooked functions; the game host registers the hook
(`games/mhfu/host/main.cpp`, `mh_overlay_prepare` reads the overlay id from the image header and activates it).

## 5b. Interrupts and timers

The vblank interrupt (30) and virtual timers are delivered by the scheduler between thread switches: handlers run in
interrupt context (no current thread, their own guest stack in the kernel partition) and must not block — a blocking call
there returns `SCE_KERNEL_ERROR_ILLEGAL_CONTEXT`. GE signal/finish callbacks use the same path.

## 5c. First-pass HLE (to be replaced)

To get past boot, several libraries are deliberately minimal and log once that they are simplified (`sceSasCore` and
`sceAtrac3plus` are real since 2026-10-17, §5g; the `sceUtility` dialogs since 2026-10-18, §5h; `sceMpeg` since
2026-10-23, §5i): modules load as HLE stubs. Audio output paces itself in
real time per channel, which keeps game timing realistic. The memory stick is always inserted: registering an
insert/eject callback (`sceIoDevctl` 0x02415821) notifies it at once with "inserted", as the system does.

## 5g. Audio (`hle_sas.cpp`, `hle_media.cpp`)

- **sceSasCore** (2026-10-17): real voice mixer. VAG (PSP ADPCM) voices with the five SPU predictors and block
  flags (7 = end, 6 = loop start, 3 = loop end when the voice was set to loop), pitch with linear interpolation
  (0x1000 = 44.1 kHz), SPU-style "simple ADSR" envelopes (linear, bent and exponential curves; rate encoding as
  documented for the SPU and cross-checked against PPSSPP), per-voice left/right volumes (0x1000 = unity), end flags.
  Not yet: reverb (effect sends are dropped), PCM/noise/ATRAC voices.
- **sceAudio**: each output call queues its block for the host mixer. Pacing keeps the stream at real-time rate:
  the game may be up to 20 ms ahead, catches up after short stalls and restarts after pauses over 100 ms. The host
  mixer jitter-buffers every channel (consumed once it holds two blocks, paused when it runs dry) so blocks of
  different sizes (256-frame voice mixer grains, 2048-frame music frames) never click.
- **sceAtrac3plus** (`hle_atrac.cpp`, 2026-10-17): real decoding with FFmpeg's ATRAC3/ATRAC3+ decoders
  (`third_party/at3_standalone`, LGPL-2.1-or-later). The RIFF header gives codec, channels, frame size, the "fact" end
  sample and encoder offset, and the "smpl" loop. The game's timeline starts after the decoder delay (0x170 samples
  for ATRAC3+, 0x45 for ATRAC3) plus the "fact" offset; DecodeData returns one frame (2048 / 1024 samples) of stereo,
  shorter at a loop end or the track end; loops honour SetLoopNum (-1 = forever). Seeks restart the decoder one frame
  early to prime the transform overlap. Only whole files in memory are supported (FU never calls AddStreamData).
  Verified on FU's menu/village music (ATRAC3, 132 kb/s): identical to the standalone decoder and to system FFmpeg up
  to polarity. FU plays its music on its own sceAudio channel and fades it with the output volumes.
- Debugging: `MHF_AUDIO_DUMP=<file>` writes the mixed output as raw 16-bit stereo 44.1 kHz.

## 5h. System dialogs (`hle_utility.cpp`, `frontend/dialog_overlay.cpp`)

**Implemented (2026-10-18)**. On the original system the message, on-screen keyboard (OSK) and savedata dialogs are
drawn by the system over the game while the game keeps calling `*Update` every frame. Here each dialog is a state
machine in the runtime: `*InitStart` reads the request, `*Update` feeds it the player's input, and the status moves
INIT (1) → RUNNING (2) → FINISHED (3) once the player answers, then SHUTDOWN (4) → NONE (0) after `*ShutdownStart`.
The frontend draws `current_dialog()` (`mhf/psp/dialog.hpp`) in the launcher's style (dimmed game, rounded dark panel,
amber accent, Liberation Sans) and feeds keys/text back with `dialog_key()`/`dialog_text()`.

- **Input**: the controller state (edge-triggered, held directions repeat after 400 ms every 90 ms, the left stick
  counts as the d-pad): Cross confirms, Circle backs out, Square deletes, Triangle types a space, Start finishes the
  keyboard. A physical keyboard types directly (ASCII; the game's name fields are ASCII), Enter = done/confirm,
  Esc = cancel, Backspace, arrows. While the keyboard is open SDL text input is on and the keyboard stops acting as a
  gamepad.
- **Message** (`pspUtilityMsgDialogParams`): mode 0x34 (0 = error code at 0x38, 1 = UTF-8 text at 0x3C), options 0x23C
  (0x10 yes/no, 0x20 OK, 0x80 no back button, 0x100 default "No"); the answer goes to 0x240 (1 = yes/OK, 2 = no,
  3 = back).
- **OSK**: field count 0x30, fields 0x34 (0x34 bytes each: description +28, initial text +32, capacity +36, output
  +40, result +44 = 0 unchanged / 1 cancelled / 2 changed, limit +48). Fields are edited one after another; an empty
  field starts with `osk_default_text` (the launcher passes the player's pseudonym with `--name`). Cancel writes back
  the initial text.
- **Savedata**: `savedata_plan()` in `hle_savedata.cpp` says what to show. Auto load/save run at once with a corner
  toast; Load/Save/Delete ask for confirmation first (the request only runs after "Yes"; "No" or back sets the common
  result 0x1C to 1 = cancelled); list modes show the slots (titles/details/dates read from each slot's `PARAM.SFO`)
  and run on the slot the player picks (`execute_savedata(..., chosen)`), with a short "Saving..." progress view.
  Requests with nothing to ask (sizes, listings, nothing to load) answer without UI. Freedom Unite draws its own slot
  menus and uses autosave/autoload, so in practice it shows toasts.
- **Drawing**: CPU raster (`DialogOverlay`, stb_truetype) into premultiplied RGBA on a worker thread whenever the view
  changes (about 30 ms at 4x), so the emulation never waits for it. The OpenGL backend composites it over the
  letterboxed game with a 150 ms fade-in (1920x1088 image); the software path blends it into the 480x272 frame.
- `RuntimeOptions::interactive_dialogs` is on only in a window (`--auto-dialogs` turns it off); headless runs and tests
  keep the old automatic answers ("yes" / OK / "back", the default text, the game's own slot).
- Network configuration (`sceUtilityNetconf*`) still reports success at once (online play, `08`).

## 5i. Movies (`hle_mpeg.cpp`, `media/`)

**Implemented (2026-10-23)**. The pre-rendered movies (intro, gallery) are Sony PSMF files: an MPEG-2 program stream of
2048-byte packs (`00 00 01 BA`) carrying H.264 pictures on stream `0xE0` and ATRAC3+ sound on private stream 1 (`0xBD`),
both with PES timestamps. Inside the game's disc they are regions of `DATA.BIN`, which is the only file the game opens.

- `media/psmf_demuxer` splits the packs into access units: pictures are delimited by the H.264 access unit delimiter
  (`00 00 01 09`), a picture is complete when the next one starts (or the stream ends); sound frames are found by their
  `0x0FD0` header and decoded without it. Timestamps come from the PES headers, and are extrapolated at the stream's
  frame rate when absent.
- Pictures are decoded by **FFmpeg's libavcodec** (H.264, one thread, `AV_CODEC_FLAG_LOW_DELAY`, so each call produces
  its picture; `MHF_FFMPEG=0` leaves the movies out, and the game skips them as it did before).
- Sound is decoded by `third_party/at3_standalone` (the same ATRAC3/ATRAC3+ code `sceAtrac3plus` uses).
- The ring buffer is the game's own memory: `sceMpegRingbufferPut` calls the game's read callback from inside the HLE
  (on the calling thread, with a copy of its registers), so the file is read by the game's own code exactly as on the
  PSP. Packs leave the ring as they are demultiplexed.
- `sceMpegAvcDecode` hands the game finished 32-bit pixels, not YCbCr: the PSP library converts with the default decode
  mode, and FU imports neither `sceMpegAvcDecodeMode` nor any CSC function. Each row is `frameWidth * 4` bytes (R, G, B, A
  in memory) and **opaque**: FU blends the frame over the black screen it clears, so a zero alpha leaves it invisible.
- Not implemented: `sceMpegAvcDecodeMode` and the YCbCr path (`AvcDecodeYCbCr`, `AvcInitYCbCr`, `QueryYCbCrSize`),
  `sceMpegAvcDecodeDetail`, `sceMpegbase`, PCM streams and seeking. FU imports none of them.
- Verified: the intro of FU EU decodes 480x272 at 29.97 Hz with its ATRAC3+ sound, from the first frame to the end.

## 5d. Savedata (`hle_savedata.cpp`)

**Implemented (2026-10-16)**: every `sceUtilitySavedata` mode (load/save/list/delete, sizes, list, files, the
multi-call read/write/erase modes, get size); the dialogs around it are §5h. A save is the folder
`<memstick>/PSP/SAVEDATA/<gameName><saveName>/` holding the game's data file exactly as the game handed it over, a
`PARAM.SFO` (titles and detail shown in lists, read back into `sfoParam` on load) and any icon/picture/sound the game
provides. Writes go through a temporary file and a rename, so a crash never leaves half a save. A save name of `<>`
means "none" (the folder is the game name alone). Where the system would show a list for the player, the slot is chosen
from the game's list and focus hint (newest existing save for loads, first empty slot for new saves) and written back to
`saveName`. Parameter offsets were checked with `offsetof()` against the pspsdk header (structure size 0x600).

**Save encryption (2026-10-22).** The original system encrypts the data file with a 16-byte key that the game passes in the
request (offset 0x5DC of `SceUtilitySavedataParam`; one key per version of the game: the European and American releases
differ), and keeps AES-CMAC hashes of it in `PARAM.SFO` (`SAVEDATA_FILE_LIST` for the file, `SAVEDATA_PARAMS` for the rest).
`third_party/pokke_save_crypto` (pokke's, MIT, with its author's permission) implements the scheme (modes 1, 3 and 5; the
console-bound 2, 4 and 6 need a PSP's fuse keys and are not supported).
- **Loading**: if the `PARAM.SFO` marks the data file as encrypted, the game's key must prove itself against the stored
  hash (otherwise `0x80110306`, "data broken": another version's save, or a damaged file), then the data is decrypted.
  Verified against a European save made by PPSSPP: the hash comes out of the key the game passes.
- **Saving**: a whole save in one call (how the game saves) is encrypted in mode 5 with the game's key, read back and
  checked before it is written (if it does not read back, it is written plain), and `PARAM.SFO` gets the hashes. The
  previous generation of the data file and `PARAM.SFO` is kept in `<SAVEDATA>/.previous/<folder>/`. A plain file written into
  a folder that was encrypted clears the encryption fields of its `PARAM.SFO`.
- `MHF_SAVE_ENCRYPTION=0` writes plain saves; `MHF_SAVEDATA_LOG=1` prints the request words around the key.
- The launcher imports saves (Settings, Saves) by copying folders; it never needs the key.
- Not supported: the console-bound modes, saves of another version of the game (their key differs and is only known to
  that version's executable; a European executable cannot read an American save), and writing a save a real PSP accepts
  (the one PARAM.SFO hash that needs a console's fuse keys is filled with `0x01` bytes, as PC tools do; PPSSPP accepts it).

`platforms/psp/tests/test_hle_savedata.cpp` covers the round trips on a Runtime without a program, and
`homebrew/savetest` the game-side calls.

## 5e. Fonts (`hle_font.cpp`)

**Implemented (2026-10-16)**: `sceLibFont` over one free font, Liberation Sans (SIL OFL 1.1, `third_party/fonts`),
rasterized with stb_truetype (`third_party/stb`). It is the only installed font and is rasterized at the size of the
original Latin font (10.125 points at 128 dpi, an 18-pixel em). Glyph images are drawn with their top-left corner at
`(xPos64, yPos64) >> 6` in 4/8/24/32-bpp buffers, keeping brighter pixels already there; metrics are in 26.6 fixed
point. The build copies the font next to each executable (`fonts/`), where the runtime looks for it when the working
directory has none. Freedom Unite draws its menus and dialog text with it. Open: metrics differ from the original font,
so some strings may be wider or narrower than their boxes `[VERIFY on more screens]`; Japanese/other scripts need a
second font. `homebrew/fonttest` covers metrics, drawing and clipping.

## 5f. Fallback interpreter

Guest code the static analysis missed runs in an interpreter (`interpreter.cpp`, VFPU in `interpreter_vfpu.cpp`, which
mirrors the recompiler's emitter case by case) instead of crashing; each such function is logged once. Setting
`MHF_INTERPRET_ONLY=1` runs everything except HLE stubs in the interpreter: every homebrew test also runs in that mode
(`homebrew_<name>_interpreted`), which keeps the two execution paths in agreement.

## 6. GE rendering (`platforms/psp/gpu`, `platforms/psp/frontend`): software reference, OpenGL 3.3; Vulkan later

**Implemented first (2026-10-16)**: a software reference renderer (`platforms/psp/gpu/ge.cpp`) that runs display lists
synchronously on submission and draws straight into guest VRAM: list control flow (jump/call/return/signal/finish,
base/offset/origin), vertex decoding (all formats, morphing, skinning), world/view/projection, lighting
(directional/point/spot, ambient/diffuse/specular, material update), near-plane clipping, perspective-correct
triangles, sprites, lines, points, textures (5650/5551/4444/8888, CLUT4/8/16/32 with shift/mask/offset, DXT1/3/5,
swizzle, clamp/wrap, nearest/bilinear), texture functions, fog, alpha/depth tests, blending (all factors and equations),
clear mode, color masks, block transfers. Not yet: stencil, color test, logic ops, dithering, Bezier/spline patches,
mipmaps, bounding-box branches, face culling (its orientation rule is `[VERIFY]`). `homebrew/getest` checks pixels.

*Clipping rule (as the PSP does it, 2026-10-17)*: triangles are clipped only against the near plane (z > -w in clip
space). There is no left/right/top/bottom clipping; instead, a triangle is discarded whole when any vertex in front of
the near plane maps (screen position + screen offset) outside the 4096×4096 drawing space. Clipping against w ≈ 0
instead produced giant stretched triangles in game scenes; translucent ones (haze layers) that write depth also hid the characters behind them for single frames, which looked like characters popping in and out.
Because it writes real VRAM, render-to-texture and framebuffer reads work by construction; the Vulkan backend will keep
the same state model and use the software path as its reference in tests.

**GE in parallel with the CPU (2026-10-17)**: as on the hardware, display lists run on a worker thread
(`hle_ge.cpp`) in submission order while the game prepares the next frame; stall addresses pause a list until
`sceGeListUpdateStallAddr`; `sceGeListSync`/`sceGeDrawSync` (mode 0) wait for the worker. Signal/finish callbacks are
queued by the worker and delivered in interrupt context. Because the game busy-waits on flags those callbacks set, the
recompiler emits `abi::poll_interrupts(rt)` on every backward branch: a host ticker (every 2 ms) and the GE worker
raise `abi::interrupt_pending`, and the poll runs pending interrupts (GE callbacks, vblank, timers) inside the guest
loop, as a real interrupt would. `MHF_GE_SYNC=1` restores synchronous lists for debugging. The renderer reads and
writes guest memory through direct pointers (no per-pixel bounds checks). Measured on a 2-core cloud VM in the
opening scene: 18 → 25 fps; the software renderer is now the bottleneck (~28 ms per frame), hence the GPU backend.
`[perf]` lines in the log report frames per second and display-list time every 5 s.

**Hardware renderer, OpenGL 3.3 (2026-10-17)**: `platforms/psp/frontend/gl_backend.cpp`, behind the
`gpu::Backend` interface (`platforms/psp/gpu/backend.hpp`). The Ge keeps list processing, vertex decoding, T&L and
near-plane clipping; screen-space primitives go to the backend, which batches them by draw state and draws them with
one ubershader (texture functions, fog, alpha test, depth range) and fixed-function depth/blend/scissor/masks.
- *Render targets*: every framebuffer address drawn to gets a color texture + depth buffer at `--scale N` (default 4,
  i.e. 1920×1088) times its PSP size; up to 48, least recently used dropped.
- *Textures*: decoded on the CPU once (CLUT, swizzle, DXT via the Ge's own decoder) and cached by parameters; the
  content hash (bytes + palette) is checked once per frame and after each palette load. A texture whose address lies in
  a framebuffer the GE has drawn samples that render target directly (render to texture); the target being drawn is
  copied first.
- *Block transfers* out of a render target are blits on the GPU; CPU pixels copied into a drawn framebuffer are uploaded.
- *Presentation*: `sceDisplaySetFrameBuf` queues a flip in order with the lists; the GE thread draws the displayed
  target letterboxed into the window and swaps (no vsync: pacing is the emulated vblank). A framebuffer the GE has not
  drawn recently (movies, CPU-made screens) is uploaded from guest memory instead.
- The GE thread owns the OpenGL context; the main thread only pumps window/input events. Without OpenGL 3.3 the
  frontend falls back to the software renderer (`--software` forces it). F11 or Alt+Enter toggles fullscreen.
- Known gaps: stencil, color test, logic ops, 2×alpha blend factors (approximated), CPU reads of framebuffers (VRAM is
  not updated; `MHF_GL_READBACK=1` copies targets back after every list, for tests), CLUT textures read from a render
  target, Bézier/spline patches. `homebrew_getest_opengl` runs the GE test through it (offscreen EGL, read back).
- Debugging: `MHF_GL_DUMP=<dir>` (+ `MHF_GL_DUMP_EVERY=n`) writes presented frames as PPM.
- *Frame interpolation* (enhancement, `--fps N`, 30 = off by default; tried on the real game at 60, 120 and 180): the
  game's logic runs once per flip at 30 fps, so a higher rate can only come from the renderer. `N` is rounded to a whole
  multiple k of 30, from 1 to 6 (30, 60, 90, 120, 150, 180). The backend keeps the batches of each flip (draw state +
  final vertices). At the next flip it draws that flip's batches again into a scratch target, starting from the previous
  picture, at phase j/k for j = 1 .. k-1, and then shows the flip itself. 3D batches are placed again from the world
  position of every vertex (the GE keeps it, `Vertex::world`) through a camera interpolated between the previous flip's
  view/projection and this one's; vertices that were drawn in the previous flip too (same guest address and same
  drawing command, so a repeated model is told apart) are first moved back toward where they were, so objects that move
  on their own are interpolated as well. Placing the points again, instead of moving the screen-space vertices of
  paired triangles, matters for terrain: the GE clips and culls it differently every flip, so triangle lists never
  match. 2D batches (through mode) are paired with the previous flip's only when they have exactly the same draw state
  (the game alternates two draw buffers, so which one a batch went to does not count), texture object and vertex count (looking ahead up to four batches), and a triangle is moved in screen space only when
  it slid rigidly (no more than 32 PSP pixels, vertices within 1.5 px of each other's displacement) with the same
  texture coordinates. Since every in-between picture starts from the previous picture, sprites that appear, vanish or
  jump would show twice for a moment (item icons, lists): when more than 6 of them change in a flip that does not clear
  the color buffer, that flip is shown as the game drew it (`menus` in the log). A flip that clears first (every 3D
  scene with its HUD) is never skipped for this. Textures are never overwritten in place while recorded batches may still use them (the old one is retired
  and deleted four flips later). Not interpolated, and shown as before: points behind the near plane, a camera that
  turned too much (cuts), batches without a partner, flips whose render target was copied into or sampled while
  being drawn, flips at an already fast pace (< 22 ms) or after a stall (> 150 ms), and the first flip after any of
  those. The in-between pictures are made only while the GE has no display list to run (`Backend::next_wakeup`/`tick`,
  honoured by the GE worker), so a heavy scene shows fewer of them instead of slowing the game down. A picture goes out at
  the next slot of 1/k of the flip interval or, if the GE was busy, as soon as it is free, and it always sits exactly
  on the slot nearest to that moment, so steady motion moves in whole shares of a flip's move; a late picture is shown
  rather than dropped. The GE runs the game's next display list right after each flip (about 10 ms in heavy scenes)
  and cannot make pictures meanwhile, which caps 180 fps at about 150 there; making the pictures on a thread of their
  own is the planned fix (pausing the game's list for them made textures flicker). A flip appears (k-1)/k of a frame late:
  17 ms at 60 fps, up to 28 ms at 180. `MHF_FPS=<n>` overrides the option, `MHF_INTERP_LOG=1` prints how often each
  case happens, the pictures put on screen per second and what a picture costs; `MHF_PROFILE=1` (Windows) samples
  where the game's thread and the drawing thread spend their time.
  `homebrew_motiontest_interpolation` checks 60, 90, 120 and 180 fps: about k pictures per flip, equal steps for shapes
  moving a fixed step per frame, and a static shape that stays put (needs pspdev; not run in the last change).
  Limits: the cost of an in-between picture grows with the geometry of the scene, so 180 fps is not sustained in the
  heaviest areas; the output has no vsync yet, so a monitor whose refresh differs from the target may show uneven
  pacing or tearing.

Why OpenGL first instead of Vulkan : the same code runs on Windows, Linux and the Steam Deck, it needs no shader
toolchain, and it was the fastest way to 30 fps at high resolution. The backend interface keeps a Vulkan
implementation possible later without touching the Ge.

Remaining components for a fuller hardware backend (Vulkan or extending this one):
1. **List interpreter**: walks the list, keeps GE state (~256 registers), detects draw calls.
2. **Vertex decoder**: converts GE formats to a fixed format; specializations per format.
3. **Shaders**: ubershader or per-state generation (T&L, lights, fog, texturing, alpha test, color test, blending). Emulate in
   shader what Vulkan lacks (ABSDIFF, 2×SRC, per-bit mask, color test, stencil-in-alpha).
4. **Texture cache**: by address + format + CLUT + hash; deswizzle; CLUT shift/mask; DXT.
5. **Framebuffer manager**: each GE framebuffer ↔ a Vulkan image; detect textures reading a framebuffer (render-to-texture), copies
   to/from RAM, depth reads. The hardest part; start with what FU needs.
6. **Scaling**: internal resolution ×N; widescreen (enhancement) later.

Behavior references: PPSSPP docs, ACX recomp's `runtime/gpu` (respect its license).

## 7. Debugging

- Overlay (ImGui, F4 like ACX) showing: FPS, threads, recent HLE calls, unimplemented NIDs, fallback-interpreter counters, active
  overlays, framebuffer/texture viewer (local only).
- File log with configurable level; "copy report" button (no game data) for testers.
