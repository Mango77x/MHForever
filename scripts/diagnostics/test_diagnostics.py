"""End-to-end checks using small generated homebrew-style inputs."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from PIL import Image
from retro_memory import ValueSearch


class Diagnostics(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def cli(self, tool, *args, status=0):
        cmd = [sys.executable, str(Path(__file__).with_name(tool + ".py")), *map(str, args)]
        result = subprocess.run(cmd, cwd=self.root, capture_output=True, text=True)
        self.assertEqual(result.returncode, status, result.stderr + result.stdout)
        return result.stdout

    def test_frames_and_sheet(self):
        for name in ("a", "b"):
            folder = self.root / name
            folder.mkdir()
            Image.new("RGB", (4, 2), "red").save(folder / "gl_001.ppm")
        self.cli("frames", "compare", "a", "b")
        self.cli("frames", "sheet", "a", "sheet.png")
        with Image.open(self.root / "sheet.png") as image:
            self.assertEqual(image.size, (240, 156))
        frame = self.root / "b/gl_001.ppm"
        Image.new("RGB", (4, 2), "blue").save(frame)
        self.assertIn("changed pixels", self.cli("frames", "compare", "a", "b", status=1))
        frame.rename(frame.with_name("gl_002.ppm"))
        self.assertIn("2 differences", self.cli("frames", "compare", "a", "b", status=1))

    def test_vram_formats_and_stride(self):
        for fmt, red, blue in (("565", 31, 31 << 11), ("5551", 31, 31 << 10),
                               ("4444", 15, 15 << 8), ("8888", 255, 255 << 16)):
            width = 4 if fmt == "8888" else 2
            raw = b"head" + b"".join(v.to_bytes(width, "little") for v in (red, 0, blue, 0))
            (self.root / "raw").write_bytes(raw)
            self.cli("vram2png", "raw", "out.png", fmt, "--width", 1, "--height", 2,
                     "--stride", 2, "--offset", 4)
            with Image.open(self.root / "out.png") as image:
                self.assertEqual(image.getpixel((0, 0)), (255, 0, 0))
                self.assertEqual(image.getpixel((0, 1)), (0, 0, 255))
        self.cli("vram2png", "raw", "out.png", "565", status=2)

    def test_vram_list_report(self):
        source = struct.pack("<2I", 0x04030000, 0x0b000000)
        for name, data in (("source", source), ("vram", source),
                           ("lists", struct.pack("<2I", 0x10040000, 0x0a000000))):
            (self.root / f"pose-{name}.bin").write_bytes(data)
        result = self.cli("analyze_vram_dump", ".", "pose", "source", "0")
        self.assertIn("8/8 bytes", result)
        self.assertIn("on a sub-list start 1", result)
        self.cli("analyze_vram_dump", ".", "pose", "source", "8", status=2)

    def test_symbols_memory_and_objects(self):
        (self.root / "map.toml").write_text("[functions]\nfirst = 0\n[data]\nitem = 1\n")
        result = json.loads(self.cli("retro_memory", "addr", "map.toml", "2", "--scope", "data"))
        self.assertEqual((result["name"], result["offset"]), ("item", 1))
        before = struct.pack("<4I", 256, 1, 256, 2)
        after = struct.pack("<4I", 256, 2, 256, 2)
        (self.root / "before").write_bytes(before)
        (self.root / "after").write_bytes(after)
        diff = json.loads(self.cli("retro_memory", "diff", "before", "after", "--base", 0))
        self.assertEqual(diff["changed_bytes"], 1)
        objects = json.loads(self.cli("retro_memory", "objects", "before", 256, "--base", 0, "--size", 8))
        self.assertEqual(objects["addresses"], [0, 8])
        self.assertEqual(ValueSearch(before, 0, 4, 1).next(after, changed=True), {4: 2})

    def test_golden_vectors(self):
        ref, actual = self.root / "ref", self.root / "actual"
        rows = ['{"case":"a","result":0}', '{"case":"b","result":1}']
        ref.write_text("\n".join(rows))
        actual.write_text("\n".join(reversed(rows)))
        self.cli("golden", ref, actual)
        actual.write_text(rows[0])
        self.cli("golden", ref, actual, status=1)
        actual.write_text("\n".join(rows + rows))
        self.cli("golden", ref, actual, status=2)

    def test_elf_report(self):
        names = b"\0.shstrtab\0.text\0"
        hdr = struct.pack("<16sHHIIIIIHHHHHH", b"\x7fELF\x01\x01\x01", 2, 8, 1,
                          0, 0, 52, 0, 52, 0, 0, 40, 3, 1)
        payload = (hdr + bytes(40)
                   + struct.pack("<10I", 1, 3, 0, 0, 172, len(names), 0, 0, 1, 0)
                   + struct.pack("<10I", 11, 1, 4, 0, 172 + len(names), 4, 0, 0, 4, 0)
                   + names + struct.pack("<I", 0x60000000))
        path = self.root / "homebrew.elf"
        path.write_bytes(payload)
        result = json.loads(self.cli("elf_report", path))
        self.assertEqual(result["static_vfpu_families"], {"VFPU0": 1})
        self.assertEqual(result["sections"][-1]["name"], ".text")
        path.write_bytes(payload[:-1])
        self.cli("elf_report", path, status=2)
