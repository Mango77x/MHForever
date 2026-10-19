# PPSSPP PRX decrypter (trimmed)

Copied verbatim (except where stated) from PPSSPP, https://github.com/hrydgard/ppsspp, on 2026-10-19.
MH-Forever is GPL-3.0-or-later; every file here can be combined under that license.

| Path | Origin | License |
|---|---|---|
| `Core/ELF/PrxDecrypter.cpp` | `Core/ELF/PrxDecrypter.cpp` lines 1-1413, with the `amctrl.h` include removed | GPL-2.0-or-later (PPSSPP; header says GPL 2.0) |
| `Core/ELF/PrxDecrypter.h` | same, minus the declarations of the DNAS/PGD/DRM-BB helpers | same |
| `ext/libkirk/kirk_engine.c/.h` | `ext/libkirk/` (Draan, KIRK-ENGINE) | GPL-3.0-or-later |
| `ext/libkirk/bn.c`, `ec.c` | `ext/libkirk/` (Segher Boessenkool) | GPL-2.0 or GPL-3.0, at your option |
| `ext/libkirk/AES.c/.h` | `ext/libkirk/` (OpenBSD rijndael) | public domain |
| `ext/libkirk/SHA1.c/.h`, `kirk_common.h` | `ext/libkirk/` | permissive (David Ireland's SHA-1; see header) |
| `shim/Common/*.h` | written for MH-Forever to replace PPSSPP's `Common/` headers (types, little-endian aliases, no-op logging) | GPL-3.0-or-later |

Not copied: `ext/libkirk/amctrl.c` (no license header; only the DNAS/PGD/DRM-BB functions, which the installer does not need).
Little-endian hosts only (the shim refuses big-endian at compile time).
