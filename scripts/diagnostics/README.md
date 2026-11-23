# Offline developer diagnostics

These optional Python tools work on local homebrew artifacts. Run the commands below from `scripts/diagnostics` with Python 3.11 or later. Only `frames.py` and `vram2png.py` need Pillow (`python -m pip install Pillow` in your development environment).

| Command | Inputs and interpretation |
| --- | --- |
| `python frames.py compare baseline candidate` | Compare decoded RGB pixels of same-named PPM, BMP or PNG frames in two directories. Reports missing files, dimension changes and the bounding box of changed pixels. For the existing motiontest, point it at two directories produced with `MHF_GL_DUMP`. Compare deterministic runs with matching capture cadence. |
| `python frames.py sheet candidate preview.png --step 5` | Create a labeled contact sheet, taking every fifth image in lexicographic filename order. Write the output outside the input directory. |
| `python vram2png.py frame.raw frame.png 565` | Decode one linear framebuffer, skipping stride padding. Accepts 565, 5551, 4444 or 8888, byte offset, dimensions and stride in pixels. Output shows RGB only. |
| `python analyze_vram_dump.py captures pose source 100` | Compare pose-source.bin to pose-vram.bin at hex offset 100. Scan pose-lists.bin for CALLs preceded by BASE within six words. The classification assumes direct addresses and linear command buffers. A word following RET is only a candidate sub-list start. |
| `python retro_memory.py diff before.raw after.raw --base 0` | Compare equal-sized byte ranges. Optional region and symbol annotations split changes at known boundaries. The JSON includes differing byte values. |
| `python retro_memory.py sym symbols.toml draw` | Search the functions and data tables of a region map by name. Use `addr symbols.toml 0x100 --scope functions` for a nearest-address lookup. Also supports scoped *.symbol_addrs.txt maps. |
| `python retro_memory.py objects snapshot.raw 0x100 --base 0 --size 16` | Find aligned candidate objects sharing a pointer and compare their fields. The Python ValueSearch class can narrow an aligned integer search over successive snapshots. |
| `python golden.py reference.jsonl actual.jsonl` | Compare integer outputs by case ID, regardless of order. Each runner writes one object per line, e.g. `{"case":"wrap-add","result":0}`. Use the same inputs and integer representation in both runners. Missing cases or differing values fail the comparison. Useful for a synthetic C implementation and an independent reference. |
| `python elf_report.py homebrew.elf` | Report file hash, entry point, section sizes and static VFPU opcode-family counts for a little-endian MIPS ELF32 with named sections. Counts cover executable PROGBITS sections and can include embedded data. |

Both comparison commands (`frames.py compare` and `golden.py`) return 0 for a match, 1 for differences and 2 for invalid input. The VRAM list analyzer is an exploratory report. Keep local captures and their outputs outside the repository.

Run `python -m unittest discover -s . -p test_diagnostics.py -v` in this directory to exercise the tools using generated data. No game installation is needed.
