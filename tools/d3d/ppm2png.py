"""ppm2png.py <in.ppm> <out.png>: binary PPM (P6) to PNG without dependencies."""
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
parts = data.split(maxsplit=4)
width, height = int(parts[1]), int(parts[2])
pixels = parts[4]
rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))
open(sys.argv[2], "wb").write(png)
