#!/usr/bin/env python3
"""Map where a recorded SSIC2P pass lost step with the planar screen.

SSIC2P.TOS writes each pass's record buffer as C2P<clock><pass>.BIN. This
rebuilds the screen it streamed (the same LCG), converts it with a direct
c2p, and walks the recording against it: wherever the recording stops
matching, it looks a few words either way for where the planar stream picks
up again and reports the slip. A slip of +1 at a word is one output slot
sent twice (the DSP's transmit was late); a garbage run without a clean
resumption is input lost (group alignment broken).
"""
import argparse
from pathlib import Path
import struct

SCREEN_BYTES = 320 * 200
ANCHOR, MATCH = 32, 8


def screen():
    state, pixels = 0x12345678, bytearray()
    for _ in range(SCREEN_BYTES):
        state = (state * 1103515245 + 12345) & 0xFFFFFFFF
        pixels.append(state >> 24)
    return pixels


def planar(pixels):
    words = []
    for group in range(0, len(pixels), 16):
        chunk = pixels[group:group + 16]
        for k in range(8):
            words.append(sum(((chunk[i] >> k) & 1) << (15 - i) for i in range(16)))
    return words


def analyse(path, want):
    data = path.read_bytes()
    got = list(struct.unpack(f">{len(data) // 2}H", data))
    anchor = want[ANCHOR:ANCHOR + MATCH]
    start = next((p for p in range(len(got) - MATCH) if got[p:p + MATCH] == anchor), None)
    if start is None:
        return f"{path.name}: planar screen not found"
    offset = start - ANCHOR
    events, i = [], 0
    while i < len(want):
        j = i + offset
        if j < len(got) and got[j] == want[i]:
            i += 1
            continue
        # where does the stream resume?
        for delta in (1, -1, 2, -2, 3, -3, 4, -4, 8, -8):
            k = i + 1
            if all(0 <= k + n + offset + delta < len(got) and got[k + n + offset + delta] == want[k + n]
                   for n in range(MATCH)):
                events.append((i, delta))
                offset += delta
                i = k
                break
        else:
            events.append((i, None))
            i += 1
            if len([e for e in events if e[1] is None]) > 20:
                break
    lines = [f"{path.name}: planar at word {start - ANCHOR}, {len(events)} events"]
    for index, delta in events[:40]:
        what = "garbage" if delta is None else f"slip {delta:+d}"
        lines.append(f"  planar word {index:6d} (group {index // 8:5d}, plane {index % 8}): {what}")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dumps", type=Path, nargs="+")
    args = parser.parse_args()
    want = planar(screen())
    for path in args.dumps:
        print(analyse(path, want))


if __name__ == "__main__":
    main()
