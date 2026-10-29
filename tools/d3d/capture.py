"""Read a frame capture (src/native_gpu/frame_capture.cpp):
    capture.py <file.sk1c>            # event list summary
    capture.py <file.sk1c> --events   # one line per event
    capture.py <file.sk1c> --passes   # render targets, resolves, draw counts
"""
import collections
import struct
import sys

NAMES = {
    0x82CB38E8: "DrawIndexedVertices", 0x82CB3500: "DrawVertices",
    0x82CB29D8: "BeginVertices", 0x82CB2E78: "EndVertices", 0x82CC7510: "ClearF",
    0x82CC7FF0: "Resolve", 0x82CBA6B0: "SetRenderTarget",
    0x82CBA070: "SetDepthStencilSurface", 0x82CC4FF0: "BeginTiling",
    0x82CC5578: "EndTiling", 0x82CC4E78: "SetPredication", 0x82CB9FB8: "SetViewport",
    0x82CB9270: "Sub_82CB9270", 0x82CCBE98: "Swap",
}


# Register groups of the device shadow (first register, device offset), from
# the draw flush (docs/native_d3d.md): register base+k is at offset + 4k.
REGISTER_GROUPS = [(0x2000, 10368, 19), (0x2100, 10444, 21), (0x2180, 10528, 5),
                   (0x2200, 10548, 12), (0x2280, 10596, 21), (0x2300, 10680, 38),
                   (0x2380, 10832, 32)]
COLOR_FORMATS = {0: "8_8_8_8", 1: "8_8_8_8_GAMMA", 2: "2_10_10_10", 3: "2_10_10_10_FLOAT",
                 4: "16_16", 5: "16_16_16_16", 6: "16_16_FLOAT", 7: "16_16_16_16_FLOAT",
                 10: "2_10_10_10_AS_10_10_10_10", 12: "2_10_10_10_FLOAT_AS_16_16_16_16",
                 14: "32_FLOAT", 15: "32_32_FLOAT"}


class Event:
    def __init__(self, function, regs, result, lr, vs, ps, device, blobs):
        self.function, self.regs, self.result, self.lr = function, regs, result, lr
        self.vs, self.ps, self.device, self.blobs = vs, ps, device, blobs
        self.name = NAMES.get(function, f"{function:08X}")

    def u32(self, offset):
        """Big-endian dword of the device shadow."""
        return struct.unpack_from(">I", self.device, offset)[0]

    def reg(self, register):
        for first, offset, count in REGISTER_GROUPS:
            if first <= register < first + count:
                return self.u32(offset + 4 * (register - first))
        raise KeyError(f"register {register:04X} is not in the device shadow")

    def regf(self, register):
        return struct.unpack(">f", struct.pack(">I", self.reg(register)))[0]

    def target(self):
        """Render target state: (pitch, msaa, color base/format, depth base/format)."""
        surface, color, depth = self.reg(0x2000), self.reg(0x2001), self.reg(0x2002)
        fmt = COLOR_FORMATS.get((color >> 16) & 0xF, str((color >> 16) & 0xF))
        return (f"pitch={surface & 0x3FFF} msaa={1 << ((surface >> 16) & 3)} "
                f"color@{color & 0xFFF}:{fmt} depth@{depth & 0xFFF}:{'D24FS8' if depth >> 16 & 1 else 'D24S8'}")

    def viewport(self):
        sx, ox, sy, oy = (self.regf(r) for r in range(0x210F, 0x2113))
        return f"viewport {abs(sx) * 2:.0f}x{abs(sy) * 2:.0f} at ({ox - abs(sx):.0f},{oy - abs(sy):.0f})"


def read(path):
    data = open(path, "rb").read()
    magic, version, device, snapshot = struct.unpack_from("<4sIII", data, 0)
    assert magic == b"SK1C" and version == 1, "not a frame capture"
    pos = 16
    events = []
    while data[pos:pos + 4] == b"EVNT":
        function, *rest = struct.unpack_from("<I8IIIQQI", data, pos + 4)
        regs, (result, lr, vs, ps, count) = rest[:8], rest[8:]
        pos += 4 + 4 + 32 + 8 + 16 + 4
        dev = data[pos:pos + snapshot]
        pos += snapshot
        blobs = []
        for _ in range(count):
            address, size = struct.unpack_from("<II", data, pos)
            blobs.append((address, data[pos + 8:pos + 8 + size]))
            pos += 8 + size
        events.append(Event(function, regs, result, lr, vs, ps, dev, blobs))
    assert data[pos:pos + 4] == b"DONE", "truncated capture"
    return events


def main():
    events = read(sys.argv[1])
    if "--passes" in sys.argv:
        state, draws = None, 0
        for i, e in enumerate(events):
            if e.name in ("DrawIndexedVertices", "DrawVertices", "EndVertices"):
                current = (e.target(), e.viewport())
                if current != state:
                    if draws:
                        print(f"      {draws} draws")
                    print(f"{i:4} {current[0]}  {current[1]}")
                    state, draws = current, 0
                draws += 1
            elif e.name in ("Resolve", "ClearF", "BeginTiling", "EndTiling"):
                if draws:
                    print(f"      {draws} draws")
                    draws = 0
                extra = ""
                if e.name == "Resolve":
                    pitch = e.reg(0x231A)
                    extra = (f"dest {e.reg(0x2319):08X} {pitch & 0x3FFF}x{(pitch >> 16) & 0x3FFF} "
                             f"format {(e.reg(0x231B) >> 7) & 0x3F} control {e.reg(0x2318):08X}")
                print(f"{i:4} {e.name} {' '.join(f'{r:08X}' for r in e.regs[1:4])} {extra}")
        if draws:
            print(f"      {draws} draws")
        return
    if "--events" in sys.argv:
        for i, e in enumerate(events):
            args = " ".join(f"{r:08X}" for r in e.regs[1:5])
            print(f"{i:4} {e.name:22} {args}  vs={e.vs:016X} ps={e.ps:016X}")
        return
    print(f"{len(events)} events")
    for name, n in collections.Counter(e.name for e in events).most_common():
        print(f"  {n:5} {name}")
    draws = [e for e in events if e.name.startswith("Draw") or e.name == "EndVertices"]
    print(f"{len(draws)} draws, {len({(e.vs, e.ps) for e in draws})} shader pairs, "
          f"{sum(1 for e in draws if not e.vs or not e.ps)} without a cached shader")


if __name__ == "__main__":
    main()
