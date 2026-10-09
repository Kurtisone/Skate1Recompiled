"""Print PE sections of a dumped XEX image: sections.py <bin>"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
n = struct.unpack_from('<H', d, pe + 6)[0]; opt = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
for i in range(n):
    o = pe + 24 + opt + 40 * i
    name = d[o:o + 8].rstrip(b'\0').decode(); vsz, va = struct.unpack_from('<II', d, o + 8)
    flags = struct.unpack_from('<I', d, o + 36)[0]
    print(f"{name:8} {base + va:08X}-{base + va + vsz:08X} flags={flags:08X}")
