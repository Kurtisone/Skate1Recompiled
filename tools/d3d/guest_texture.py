"""Decode a tiled 8_8_8_8 texture from a capture's physical memory dump:
    guest_texture.py <capture.sk1c.phys> <address hex> <width> <height> <pitch> <out.ppm>
(the emulated GPU's resolves land there, a reference for the native ones)."""
import sys


def tiled_offset(x, y, pitch, log2):
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


phys, address, width, height, pitch, out = sys.argv[1], int(sys.argv[2], 16), *map(int, sys.argv[3:6]), sys.argv[6]
with open(phys, "rb") as f:
    f.seek(address & 0x1FFFFFFF)
    data = f.read(((pitch + 31) & ~31) * ((height + 31) & ~31) * 4 + 4096)
rows = bytearray()
for y in range(height):
    for x in range(width):
        o = tiled_offset(x, y, pitch, 2)
        b = data[o:o + 4]  # big-endian A8R8G8B8 word -> bytes A R G B
        rows += bytes((b[1], b[2], b[3]))
open(out, "wb").write(b"P6\n%d %d\n255\n" % (width, height) + bytes(rows))
