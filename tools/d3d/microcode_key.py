"""Shader microcode of an XDK shader container (the bytes the runtime finds at
the shader object's microcode address; Shader.size is in bytes): microcode_key.py <dir> [<dir>...]
prints sha1(microcode)[:16] and the file for every container."""
import hashlib
import struct
import sys
from pathlib import Path


def microcode(data):
    vsize = struct.unpack_from(">I", data, 4)[0]
    shader_offset = struct.unpack_from(">I", data, 0x18)[0]
    phys_offset, size = struct.unpack_from(">II", data, shader_offset)
    start = vsize + phys_offset
    return data[start:start + size]


if __name__ == "__main__":
    for d in sys.argv[1:]:
        for f in sorted(Path(d).glob("*.bin")):
            print(hashlib.sha1(microcode(f.read_bytes())).hexdigest()[:16], f)
