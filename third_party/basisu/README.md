# basisu

The transcoder of Basis Universal (`transcoder/`) and zstd (`zstd/`, only the files the transcoder includes), from
https://github.com/BinomialLLC/basis_universal at commit 073eb6a0a34a23d77e75e54f41dcd9653a52da83, unmodified.

- Basis Universal: Apache License 2.0 (`LICENSE`).
- zstd: BSD-3-Clause or GPL-2.0, used here under BSD-3-Clause (`zstd/LICENSE`).

Used by the developer texture-pack replacement (`MHF_TEXTURE_PACK`) to read the pack's `.ktx2` files (UASTC with zstd
supercompression) and turn them into RGBA8. Not used when no pack is set.
