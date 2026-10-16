# Third-party components

Every dependency included in `third_party/` is listed here with its license, version/commit and origin.
Read `docs/02-architecture.md` §9 before adding anything.

| Component | Path | License | Version | Origin |
|---|---|---|---|---|
| PSPRecomp framework | `third_party/psprecomp/` | MIT | `f6e7d41` (2026-08-13) | https://github.com/jessicanataliagta/PSPRecomp |
| PSPRecomp instruction emitter (derived) | `platforms/psp/recomp/instruction_emitter.cpp`, branch conditions in `emitter.cpp` | MIT | `f6e7d41` | moved out of PSPRecomp's `tools/codegen_main.cpp`, notice kept in the file |

Generated data: `platforms/psp/hle/nids.csv` (library, NID, function name) is extracted by `scripts/extract_nids.py` from the
PSPSDK stub libraries (pspdev, BSD-licensed). It contains identifiers only, no SDK code.

Knowledge sources (no code copied): `tclamb/mhp2g-decomp` (CC0) documented the DATA.BIN header layout, the scrambling scheme and
the `MWo3` overlay header used by `games/mhfu/tools/`. The game's substitution table itself is never included.

Never include: Sony PGF fonts, firmware, keys, decryption code, game assets.
