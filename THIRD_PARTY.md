# Third-party components

Every dependency included in `third_party/` is listed here with its license, version/commit and origin.
Read `docs/02-architecture.md` §9 before adding anything.

| Component | Path | License | Version | Origin |
|---|---|---|---|---|
| PSPRecomp framework | `third_party/psprecomp/` | MIT | `f6e7d41` (2026-08-13) | https://github.com/jessicanataliagta/PSPRecomp |

Knowledge sources (no code copied): `tclamb/mhp2g-decomp` (CC0) documented the DATA.BIN header layout, the scrambling scheme and
the `MWo3` overlay header used by `games/mhfu/tools/`. The game's substitution table itself is never included.

Never include: Sony PGF fonts, firmware, keys, decryption code, game assets.
