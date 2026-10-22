# pokke save-data cryptography

`save_crypto/` is the PSP save-data encryption (the firmware's `sceChnnlsv` scheme: AES-128 and AES-CMAC with the
PSP's fixed key slots) written by the **pokke** project (by theguyser; MIT, "Copyright (c) 2026
pokke contributors"), taken from `profiles/mhp2g/host/save_crypto/` unmodified, with the permission of its author. The
full license text is in `LICENSE` (pokke's: it also carries the PSPRecomp and Yakumo notices).

It handles save data and nothing else: it decrypts and encrypts the data file of a save, and fills the hashes of its
`PARAM.SFO`. Modes 1, 3 and 5 (fixed keys) are exact; the console-bound variants (2, 4, 6) need a PSP's own fuse keys
and are not supported. We use it to read saves made by a PSP or by PPSSPP (`docs/05-runtime-hle.md` §5d).

Used by `platforms/psp/runtime` (loading saves) and the launcher (importing them). The tests are in
`platforms/psp/tests/test_save_crypto.cpp`; the AES and CMAC code is checked there against the FIPS-197 and RFC 4493
test vectors.
