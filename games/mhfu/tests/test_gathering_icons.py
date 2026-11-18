"""Exercise the icon preparation executable with synthetic PNG pixels only."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib


def png(path, width, height):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # PNG filter: none
        for x in range(width):
            rows.extend((x % 256, y % 256, 200, 128))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


with tempfile.TemporaryDirectory() as temporary:
    root = pathlib.Path(temporary)
    atlas, output = root / 'atlas.png', root / 'icons'
    png(atlas, 256, 256)
    subprocess.run([sys.argv[1], str(atlas), str(atlas), str(output)], check=True)
    for name, x, y, size in [('background', 220, 224, 32), ('circle', 0, 0, 16),
                             ('mining', 24, 48, 24), ('bugs', 144, 24, 24), ('gather', 48, 96, 24)]:
        actual = (output / (name + '.rgba')).read_bytes()
        expected = bytearray(b'MHFICON1' + struct.pack('<I', size))
        for row in range(y, y + size):
            for column in range(x, x + size):
                expected.extend(((column * 128 + 127) // 255, (row * 128 + 127) // 255, 100, 128))
        if actual != expected:
            raise AssertionError('unexpected crop/header/premultiplication for ' + name)
    png(atlas, 256, 128)
    result = subprocess.run([sys.argv[1], str(atlas), str(atlas), str(root / 'invalid')], capture_output=True)
    if result.returncode == 0 or (root / 'invalid').exists():
        raise AssertionError('invalid atlas accepted or output written before validation')
