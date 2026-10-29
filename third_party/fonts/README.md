# Fonts

Free fonts used in place of the system fonts of the original hardware (which are never included, see CONTRIBUTING.md).

| Folder | Font | Version | License | Source |
|---|---|---|---|---|
| `liberation-sans/` | Liberation Sans Regular | 2.1.5 | SIL Open Font License 1.1 (`LICENSE`) | https://github.com/liberationfonts/liberation-fonts (TTF as distributed by Debian `fonts-liberation` 1:2.1.5) |
| `noto-sans-jp/` | Noto Sans JP Regular (the Japanese text sceLibFont draws where Liberation Sans has no glyph). Only the licence is stored here: the 4.5 MB font is downloaded by the build and checked against its SHA-256 (`cmake/fonts.cmake`) | 2.004 | SIL Open Font License 1.1 (`LICENSE`, copied as `LICENSE-NotoSansJP`) | https://github.com/notofonts/noto-cjk, tag `Sans2.004`, `Sans/SubsetOTF/JP/NotoSansJP-Regular.otf` |

The build copies these files next to each executable, into `fonts/`.
