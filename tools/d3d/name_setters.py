"""For each traced D3D entry point, list the device shadow registers it
writes (device = r3, also through `mr rN,r3`), using the draw flush's
offset -> register groups."""
import re
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from disasm import functions, registers

GROUPS = [(1920, 0x4000, 256 * 4), (6016, 0x4400, 256 * 4), (10548, 0x2200, 12),
          (10528, 0x2180, 5), (10368, 0x2000, 19), (10444, 0x2100, 26),
          (10596, 0x2280, 21), (10680, 0x2300, 38), (10832, 0x2380, 8),
          (10112, 0x4900, 64)]


def reg_for_offset(off):
    for base_off, base_reg, count in GROUPS:
        if base_off <= off < base_off + 4 * count and (off - base_off) % 4 == 0:
            return base_reg + (off - base_off) // 4
    return None


def device_stores(body):
    dev = {"r3"}
    out = []
    for l in body:
        m = re.match(r"mr (r\d+),(r\d+)", l)
        if m and m.group(2) in dev:
            dev.add(m.group(1))
        m = re.match(r"(stw|sth|stb|std) r\d+,(-?\d+)\((r\d+)\)", l)
        if m and m.group(3) in dev:
            out.append(int(m.group(2)))
    return out


def main():
    funcs = functions()
    names = registers()
    trace = [l.split() for l in open(sys.argv[1]) if l[0] not in "S#"]
    for row in sorted(trace, key=lambda r: -float(r[1])):
        addr = int(row[0], 16)
        body = funcs.get(addr, [])
        offs = device_stores(body)
        regs = sorted({reg_for_offset(o) for o in offs} - {None})
        other = sorted({o for o in offs if reg_for_offset(o) is None and o >= 0})
        regtxt = ",".join(names.get(r, f"0x{r:04X}") for r in regs[:6])
        print(f"{row[0]} {float(row[1]):7.1f}/f len={len(body):4d} regs=[{regtxt}] other_offs={other[:8]}")


if __name__ == "__main__":
    main()
