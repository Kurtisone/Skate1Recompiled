#!/usr/bin/env python3
"""Turn an audio_dump_path recording into a stereo WAV you can listen to.

  tools/audio_dump_to_wav.py DUMP OUT.wav

The dump holds the guest's 5.1 frames exactly as the game mixed them (256
big-endian floats per channel, channels in sequence). They are folded to
stereo with the SDK's default weights (center and surrounds at -3 dB, LFE
dropped, normalized) and written as 48 kHz 16-bit PCM. If this WAV plays
clean while the device crackles, the crackle comes from the output path,
not from the game's mix.
"""

import array
import struct
import sys

FRAME = 256
CENTER = SURROUND = 0.70710678
SCALE = 0.58578644


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 1
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    frames = len(data) // (FRAME * 6 * 4)
    samples = array.array("f")
    samples.frombytes(data[:frames * FRAME * 6 * 4])
    if sys.byteorder == "little":
        samples.byteswap()
    out = array.array("h")
    for frame in range(frames):
        base = frame * FRAME * 6
        fl, fr, fc, _, bl, br = (samples[base + c * FRAME:base + (c + 1) * FRAME]
                                 for c in range(6))
        for i in range(FRAME):
            for v in (fl[i] + CENTER * fc[i] + SURROUND * bl[i],
                      fr[i] + CENTER * fc[i] + SURROUND * br[i]):
                out.append(int(max(-1.0, min(1.0, v * SCALE)) * 32767))
    if sys.byteorder == "big":
        out.byteswap()
    pcm = out.tobytes()
    with open(sys.argv[2], "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, 48000, 48000 * 4, 4, 16))
        f.write(b"data" + struct.pack("<I", len(pcm)) + pcm)
    print(f"{frames * FRAME / 48000:.1f} s -> {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
