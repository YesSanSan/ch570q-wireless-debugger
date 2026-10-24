"""Render a 128x64 SSD1306 page-layout RAM dump as a PNG (Python stdlib only)."""
import argparse
from pathlib import Path
import struct
import zlib


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    data = args.input.read_bytes()
    if len(data) != 1024:
        raise ValueError('Expected exactly 1024 framebuffer bytes')
    scale = 4
    width, height = 128 * scale, 64 * scale
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            pixel = (data[(y // scale // 8) * 128 + x // scale] >> (y // scale % 8)) & 1
            rows.append(255 if pixel else 0)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
    args.output.write_bytes(png)
    print(f'Rendered SWD RAM dump: {args.output}')


if __name__ == '__main__':
    main()
