"""Extract an Xbox 360 XDVDFS game ISO (the user's own disc image).
extract_iso.py <iso> <out_dir> [--list]"""
import os, struct, sys

SECTOR = 2048
OFFSETS = (0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000)
MAGIC = b"MICROSOFT*XBOX*MEDIA"

def main():
    iso, out = sys.argv[1], sys.argv[2]
    list_only = "--list" in sys.argv
    f = open(iso, "rb")
    for base in OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            break
    else:
        sys.exit("no XDVDFS volume found")
    root_sector, root_size = struct.unpack("<II", f.read(8))
    print(f"game partition at 0x{base:X}, root sector {root_sector}, size {root_size}")
    total = 0
    pending = [(base + root_sector * SECTOR, 0, "")]
    while pending:
        dir_off, node, prefix = pending.pop()
        f.seek(dir_off + node)
        hdr = f.read(14)
        left, right, sector, length, attr, nlen = struct.unpack("<HHIIBB", hdr)
        if left == 0xFFFF and right == 0xFFFF:
            continue
        name = f.read(nlen).decode("latin1")
        if left:
            pending.append((dir_off, left * 4, prefix))
        if right:
            pending.append((dir_off, right * 4, prefix))
        path = prefix + name
        if attr & 0x10:
            if length:
                pending.append((base + sector * SECTOR, 0, path + "/"))
            if not list_only:
                os.makedirs(os.path.join(out, path), exist_ok=True)
            continue
        total += length
        if list_only:
            print(f"{length:>12} {path}")
            continue
        dst = os.path.join(out, path)
        os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
        if os.path.exists(dst) and os.path.getsize(dst) == length:
            continue
        f.seek(base + sector * SECTOR)
        with open(dst, "wb") as o:
            left_bytes = length
            while left_bytes:
                chunk = f.read(min(left_bytes, 8 << 20))
                o.write(chunk)
                left_bytes -= len(chunk)
    print(f"{total / 2**30:.2f} GiB of files")

if __name__ == "__main__":
    main()
