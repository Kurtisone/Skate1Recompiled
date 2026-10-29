"""Extract every XDK shader container embedded in a file (as XenosRecomp's
directory mode finds them) into separate files: split_shaders.py <in> <outdir>."""
import struct
import sys
from pathlib import Path


def main():
    data = Path(sys.argv[1]).read_bytes()
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    i = count = 0
    while i + 0x28 < len(data):
        flags, vsize, psize = struct.unpack_from(">III", data, i)
        field1c, field20 = struct.unpack_from(">II", data, i + 0x1C)
        size = vsize + psize
        if (flags & 0xFFFFFF00) == 0x102A1100 and size <= len(data) - i and field1c == 0 and field20 == 0:
            kind = "ps" if (flags & 1) == 0 else "vs"
            (out / f"{i:08X}_{kind}.bin").write_bytes(data[i:i + size])
            count += 1
            i += size
        else:
            i += 4
    print(f"{count} containers")


if __name__ == "__main__":
    main()
