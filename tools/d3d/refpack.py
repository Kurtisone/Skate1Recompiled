"""EA BIG archive reader with RefPack (QFS, 0x10FB) decompression."""
import struct


def refpack_decompress(src):
    pos = 0
    flags = src[0]
    if src[1] != 0xFB:
        raise ValueError("not RefPack")
    pos = 2
    size_bytes = 4 if flags & 0x80 else 3
    if flags & 0x01:  # compressed size present
        pos += size_bytes
    out_size = int.from_bytes(src[pos:pos + size_bytes], "big")
    pos += size_bytes
    out = bytearray(out_size)
    o = 0
    n = len(src)
    while pos < n:
        b0 = src[pos]
        if b0 < 0x80:
            b1 = src[pos + 1]
            plain = b0 & 3
            copy = ((b0 >> 2) & 7) + 3
            dist = ((b0 & 0x60) << 3) + b1 + 1
            pos += 2
        elif b0 < 0xC0:
            b1, b2 = src[pos + 1], src[pos + 2]
            plain = b1 >> 6
            copy = (b0 & 0x3F) + 4
            dist = ((b1 & 0x3F) << 8) + b2 + 1
            pos += 3
        elif b0 < 0xE0:
            b1, b2, b3 = src[pos + 1], src[pos + 2], src[pos + 3]
            plain = b0 & 3
            copy = ((b0 & 0x0C) << 6) + b3 + 5
            dist = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
            pos += 4
        elif b0 < 0xFC:
            plain = ((b0 & 0x1F) << 2) + 4
            copy = 0
            dist = 0
            pos += 1
        else:
            plain = b0 & 3
            copy = 0
            dist = 0
            pos += 1
        out[o:o + plain] = src[pos:pos + plain]
        o += plain
        pos += plain
        for _ in range(copy):
            out[o] = out[o - dist]
            o += 1
        if b0 >= 0xFC:
            break
    return bytes(out[:o])


def big_entries(path):
    with open(path, "rb") as f:
        data = f.read()
    count, _ = struct.unpack_from(">II", data, 8)
    pos = 16
    for _ in range(count):
        off, size = struct.unpack_from(">II", data, pos)
        end = data.index(b"\0", pos + 8)
        name = data[pos + 8:end].decode(errors="replace")
        pos = end + 1
        blob = data[off:off + size]
        if len(blob) > 2 and blob[1] == 0xFB and blob[0] in (0x10, 0x11, 0x90, 0x91):
            try:
                blob = refpack_decompress(blob)
            except Exception:
                pass
        yield name, blob
