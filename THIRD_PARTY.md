# Third-party components

Every dependency included in `third_party/` is listed here with its license, version/commit and origin.
Read `docs/02-architecture.md` §9 before adding anything.

| Component | Path | License | Version | Origin |
|---|---|---|---|---|
| PSPRecomp framework | `third_party/psprecomp/` | MIT | `f6e7d41` (2026-08-13) | https://github.com/jessicanataliagta/PSPRecomp |
| PSPRecomp instruction emitter (derived) | `platforms/psp/recomp/instruction_emitter.cpp`, branch conditions in `emitter.cpp` | MIT | `f6e7d41` | moved out of PSPRecomp's `tools/codegen_main.cpp`, notice kept in the file |
| stb_truetype | `third_party/stb/` | MIT or public domain | v1.26 | https://github.com/nothings/stb |
| PRX decrypter (PPSSPP `Core/ELF/PrxDecrypter`, trimmed) + libkirk | `third_party/ppsspp_prx/` | GPL-2.0-or-later (PrxDecrypter), GPL-3.0-or-later (`kirk_engine`), GPL-2.0-or-later or GPL-3.0-or-later (`bn.c`, `ec.c`), public domain (`AES.c`), permissive (`SHA1.c`) | PPSSPP master, 2026-10-19 (`ext/libkirk`, `Core/ELF/PrxDecrypter.*`) | https://github.com/hrydgard/ppsspp. See `third_party/ppsspp_prx/NOTICE.md`. Used only by the launcher's executable preparation |
| PSP save-data cryptography (AES-128, AES-CMAC, the `sceChnnlsv` scheme) | `third_party/pokke_save_crypto/` | MIT | pokke `profiles/mhp2g/host/save_crypto/` (2026-10-20 handoff), unmodified | theguyser's pokke project, which is not public (used with its author's permission). See `third_party/pokke_save_crypto/NOTICE.md`. Used only to read and write the data of saves |
| Liberation Sans Regular (font) | `third_party/fonts/liberation-sans/` | SIL OFL 1.1 | 2.1.5 | https://github.com/liberationfonts/liberation-fonts (TTF as packaged by Debian `fonts-liberation` 1:2.1.5) |

Fetched at build time (not stored in the repository):

| Component | Used by | License | Version | Origin |
|---|---|---|---|---|
| FFmpeg licence and source note | `third_party/ffmpeg/` | LGPL-2.1-or-later | 7.1.5 (`FFmpeg-SOURCE.txt`) | `FFmpeg-LICENSE.txt` is FFmpeg's own `COPYING.LGPLv2.1`; the source note records the version, the tarball and its SHA-256 and the configure flags. Only these two files are stored here, never a binary of the library |
| FFmpeg (libavcodec, libavutil) | `platforms/psp/runtime/src/media/` (`avc_decoder.cpp`: H.264 pictures of the movies; `atrac_decoder.cpp`: their ATRAC3+ sound) and `platforms/psp/runtime/src/hle_atrac.cpp` (the music of the game) | LGPL-2.1-or-later, never GPL and never non-free | On Linux, the system's build (pkg-config, `MHF_FFMPEG=system`, the default there); release archives and Windows pin 7.1.5 (SHA-256 in `cmake/ffmpeg.cmake`) | https://ffmpeg.org. The pinned build is configured LGPL-only by `cmake/ffmpeg.cmake`, and the configuration is checked after configure (`License: LGPL version 2.1 or later`, `CONFIG_GPL 0`, `CONFIG_NONFREE 0`) so a GPL or non-free build fails the build instead of shipping: `--enable-static --disable-shared --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample --disable-network --disable-autodetect --disable-everything --enable-decoder=h264 --enable-decoder=atrac3 --enable-decoder=atrac3p --disable-x86asm --disable-debug`. Replaced the `at3_standalone` extract (removed 2026-10-24) with the same upstream decoders |
| webview | `launcher/` (system web view host) | MIT | 0.12.0 (`3ab4b5d`, `cmake/webview.cmake`) | https://github.com/webview/webview |
| WebView2 SDK headers | `launcher/` on Windows (headers only; the runtime is part of Windows) | BSD-3-Clause | 1.0.1150.38 | NuGet `Microsoft.Web.WebView2` |
| React, React DOM | launcher UI, bundled into the launcher executable | MIT | 19.3.1 (`launcher/ui/package-lock.json`) | https://react.dev |
| Motion | launcher UI animations, bundled | MIT | 14.3.0 | https://motion.dev |
| Sora, Manrope (fonts via Fontsource) | launcher UI, bundled | SIL OFL 1.1 | 5.3.0 | https://fontsource.org |
| Vite, vite-plugin-singlefile, TypeScript | build tools for the launcher UI (not shipped) | MIT / Apache-2.0 | see `package.json` | npm |
| SDL3 | `core/host` (window, input, audio) | zlib | 3.2.24 (SHA-256 pinned in `cmake/sdl3.cmake`) | https://github.com/libsdl-org/SDL |
| pspdev toolchain + PSPSDK | builds our own `homebrew/` tests only | GPL/BSD (tools), BSD (SDK) | latest release | https://github.com/pspdev/pspdev |

Generated data: `platforms/psp/hle/nids.csv` (library, NID, function name) is extracted by `scripts/extract_nids.py` from the
PSPSDK stub libraries (pspdev, BSD-licensed). It contains identifiers only, no SDK code.

Knowledge sources (no code copied): `tclamb/mhp2g-decomp` (CC0) documented the DATA.BIN header layout, the scrambling scheme and
the `MWo3` overlay header used by `games/mhfu/tools/`. The game's substitution table itself is never included.

Known-how sources for the movie player (no code copied): PPSSPP's `sceMpeg` and `MediaEngine` (GPL-2.0-or-later) established
the PSMF/MPEG-PS layout, the ring buffer fields, the access unit structure and the pixel layout `sceMpegAvcDecode` produces;
the MHP3rd profile of the Yakumo project (private, used with its author's permission) showed the same flow end to end.
`platforms/psp/runtime/src/hle_mpeg.cpp` and `src/media/psmf_demuxer.cpp` are our own implementation, written against
the player's own files and those references, and say so in their headers.

Never include: Sony PGF fonts, firmware, game assets. The executable preparation module (`launcher/native/install/`) is ours; its decryption comes from
`third_party/ppsspp_prx/`. The installer decrypts the player's own disc at install time, so no decrypted game code is stored here.
