"""Extract every XDK shader container from the game's BIG archives
(decompressing RefPack entries): harvest_shaders.py <game/data> <outdir>.
Containers are written once each, named by archive entry and offset."""
import glob
import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from refpack import big_entries


def containers(blob):
    i = 0
    while True:
        i = blob.find(b"\x10\x2a\x11", i)
        if i < 0 or i + 0x28 > len(blob):
            return
        flags, vsize, psize = struct.unpack_from(">III", blob, i)
        size = vsize + psize
        if ((flags & 0xFFFFFF00) == 0x102A1100 and 0 < size <= len(blob) - i
                and struct.unpack_from(">II", blob, i + 12) == (0, 0x24)):
            yield i, blob[i:i + size]
            i += size
        else:
            i += 1


def main():
    root, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    seen = set()
    for path in sorted(glob.glob(str(root / "**/*.big"), recursive=True)):
        if "/audio/" in path:
            continue
        found = 0
        for name, blob in big_entries(path):
            for offset, data in containers(blob):
                digest = hashlib.sha1(data).hexdigest()
                if digest in seen:
                    continue
                seen.add(digest)
                kind = "ps" if (data[3] & 1) == 0 else "vs"
                (out / f"{digest[:16]}_{kind}.bin").write_bytes(data)
                found += 1
        print(f"{path}: {found} new containers", flush=True)
    print(f"total {len(seen)}")


if __name__ == "__main__":
    main()
