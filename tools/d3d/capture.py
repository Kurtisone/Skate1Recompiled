"""Read a frame capture (src/native_gpu/frame_capture.cpp):
    capture.py <file.sk1c>            # event list summary
    capture.py <file.sk1c> --events   # one line per event
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


class Event:
    def __init__(self, function, regs, result, lr, vs, ps, device, blobs):
        self.function, self.regs, self.result, self.lr = function, regs, result, lr
        self.vs, self.ps, self.device, self.blobs = vs, ps, device, blobs
        self.name = NAMES.get(function, f"{function:08X}")

    def u32(self, offset):
        """Big-endian dword of the device shadow."""
        return struct.unpack_from(">I", self.device, offset)[0]


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
