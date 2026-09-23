#!/usr/bin/env python3
"""Generate the DSP c2p group conversion, and check it before it is assembled.

One group is 16 chunky pixels, 8 bits each, arriving as eight 16-bit SSI
words (pixel 2j in the high byte of word j), and leaves as eight 16-bit plane
words (bit 15 - i of plane k is bit k of pixel i). On the DSP each word sits
in bits 23..8 of a 24-bit word, as the SSI receives and transmits it.

The conversion is a bit-matrix transpose in four merge stages of four
merges each. A merge of words A and B with shift s and mask m is

    t = ((A >> s) ^ B) & m;  B ^= t;  A ^= t << s

and swaps one bit of the word index with one bit of the bit position:

    stage  shift  mask     pairs words differing in
      1      8    $00ff00   4
      2      4    $0f0f00   2
      3      2    $333300   1
      4      1    $555500   4

after which plane k sits in working word (7, 3, 6, 2, 5, 1, 4, 0)[k]. The
right shift is a fractional multiply by 2^(23 - s), whose sign-filled top
bits the mask clears; the left shift is s ASLs of t moved cleanly into b.

The merges are unrolled and software-pipelined: each loads the next merge's
A in its last instruction and stores its own A' in the next merge's
multiply. The merge order within each stage is chosen so that no merge reads
a word whose store is still pending, which check() verifies along with the
result: it runs the emitted instructions on a model of the registers they
touch, for random groups and single-bit ones, against a direct c2p.

Registers on entry: r1 and r2 point at input words 3 and 7 of the group in
the X input ring, r4 and r5 at output words 0 and 1 in the Y output ring,
n4 = n5 = 2, and the rings' modifiers are set. y0, y1, x0, x1, a and b are
clobbered. The working words are X:$20-$27 and the pending-store dummy X:$3f.
"""
import argparse
import random
from pathlib import Path

SHIFTS = (8, 4, 2, 1)
MASKS = {8: 0x00FF00, 4: 0x0F0F00, 2: 0x333300, 1: 0x555500}
PARTNER = {8: 4, 4: 2, 2: 1, 1: 4}
# (A, B) working word pairs per stage, in emission order
ORDER = {
    8: [(7, 3), (6, 2), (5, 1), (4, 0)],
    4: [(7, 5), (6, 4), (3, 1), (2, 0)],
    2: [(7, 6), (3, 2), (5, 4), (1, 0)],
    1: [(7, 3), (6, 2), (5, 1), (4, 0)],
}
PLANE_WORD = (7, 3, 6, 2, 5, 1, 4, 0)
WORK = 0x20
DUMMY = 0x3F


def work(w):
    return f"x:<${WORK + w:02x}"


def merges():
    """(shift, A source, B source, A' destination, B' destination) in order."""
    out = []
    for shift in SHIFTS:
        for a, b in ORDER[shift]:
            assert a == b | PARTNER[shift]
            if shift == 8:
                a_src, b_src = "x:(r2)-", "x:(r1)-"
            else:
                a_src, b_src = work(a), work(b)
            if shift == 1:
                a_dst = "y:(r4)+n4" if PLANE_WORD.index(a) % 2 == 0 else "y:(r5)+n5"
                b_dst = "y:(r4)+n4" if PLANE_WORD.index(b) % 2 == 0 else "y:(r5)+n5"
            else:
                a_dst, b_dst = work(a), work(b)
            out.append((shift, a_src, b_src, a_dst, b_dst, a, b))
    return out


def emit():
    lines = []
    seq = merges()
    lines.append(f"        move    {seq[0][1]},x0")
    pending = f"x:<${DUMMY:02x}"
    for index, (shift, a_src, b_src, a_dst, b_dst, a, b) in enumerate(seq):
        if index == 0 or seq[index - 1][0] != shift:
            lines.append(f"; stage: shift {shift}, words {a}/{b} first")
            lines.append(f"        move    #>${1 << (23 - shift):06x},y0")
            lines.append(f"        move    #>${MASKS[shift]:06x},y1")
        lines.append(f"        move    {b_src},x1")
        lines.append(f"        mpy     x0,y0,a b1,{pending}")
        lines.append(f"        eor     x1,a")
        lines.append(f"        and     y1,a")
        lines.append(f"        eor     x1,a    a1,b")
        lines.append(f"        asl     b       a1,{b_dst}")
        for _ in range(shift - 1):
            lines.append(f"        asl     b")
        if index + 1 < len(seq):
            lines.append(f"        eor     x0,b    {seq[index + 1][1]},x0")
        else:
            lines.append(f"        eor     x0,b")
        pending = a_dst
    lines.append(f"        move    b1,{pending}")
    return lines


# ---------------------------------------------------------------------------
# A model of the emitted instructions, just the semantics they rely on.
# ---------------------------------------------------------------------------
M24, M56 = (1 << 24) - 1, (1 << 56) - 1


def signed(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


class Machine:
    def __init__(self, group):
        self.reg = {name: 0 for name in ("x0", "x1", "y0", "y1")}
        self.acc = {"a": 0, "b": 0}
        self.x, self.y = {}, {}
        # input ring at X:$40, the group at $48..$4f; output ring at Y:$40
        self.ring = 0x40
        for j, word in enumerate(group):
            self.x[0x48 + j] = word << 8
        self.ptr = {"r1": 0x4B, "r2": 0x4F, "r4": 0x48, "r5": 0x49}
        self.n = {"r4": 2, "r5": 2}

    def part(self, acc, which):
        value = self.acc[acc]
        return {"1": (value >> 24) & M24, "0": value & M24}[which]

    def address(self, operand):
        space, mode = operand.split(":")
        mem = self.x if space == "x" else self.y
        if mode.startswith("<$"):
            return mem, int(mode[2:], 16), None
        reg = mode[1:3]
        address = self.ptr[reg]
        post = mode[4:]
        step = {"+": 1, "-": -1, "+n4": 2, "+n5": 2}[post]
        return mem, address, (reg, step)

    def read(self, operand):
        mem, address, update = self.address(operand)
        self.touch(update)
        return mem.get(address, 0)

    def write(self, operand, value):
        mem, address, update = self.address(operand)
        mem[address] = value & M24
        self.touch(update)

    def touch(self, update):
        if update:
            reg, step = update
            self.ptr[reg] = self.ring + ((self.ptr[reg] - self.ring + step) % 32)

    def source(self, name):
        if name in self.reg:
            return self.reg[name]
        return self.part(name[0], name[1])

    def run(self, lines):
        for line in lines:
            if line.startswith(";"):
                continue
            fields = line.split()
            op, rest = fields[0], fields[1:]
            # the parallel move reads its source before the ALU op writes
            move = rest[1] if len(rest) > 1 else None
            staged = None
            if op != "move" and move:
                src, dst = move.split(",", 1)
                value = self.source(src) if ":" not in src else self.read(src)
                staged = (dst, value)
            if op == "move":
                src, dst = rest[0].split(",", 1)
                if src.startswith("#>$"):
                    value = int(src[3:], 16)
                elif ":" in src:
                    value = self.read(src)
                else:
                    value = self.source(src)
                self.store(dst, value)
            elif op == "mpy":
                s1, s2, d = rest[0].split(",")
                product = signed(self.reg[s1], 24) * signed(self.reg[s2], 24) * 2
                self.acc[d] = product & M56
            elif op in ("eor", "and"):
                s, d = rest[0].split(",")
                high = self.part(d, "1")
                high = high ^ self.reg[s] if op == "eor" else high & self.reg[s]
                self.acc[d] = (self.acc[d] & ~(M24 << 24) & M56) | (high << 24)
            elif op == "asl":
                self.acc[rest[0]] = (self.acc[rest[0]] << 1) & M56
            else:
                raise SystemExit(f"model: unknown instruction {line!r}")
            if staged:
                self.store(*staged)

    def store(self, dst, value):
        if dst in self.reg:
            self.reg[dst] = value & M24
        elif dst in self.acc:
            # a data register into an accumulator: a1 = value, a0 = 0, a2 = sign
            self.acc[dst] = (signed(value, 24) << 24) & M56
        else:
            self.write(dst, value)


def reference(pixels):
    return [sum(((pixels[i] >> k) & 1) << (15 - i) for i in range(16)) for k in range(8)]


def check(lines):
    rng = random.Random(2026)
    groups = [[rng.randrange(256) for _ in range(16)] for _ in range(2000)]
    groups += [[(1 << k) if i == n else 0 for i in range(16)] for n in range(16) for k in range(8)]
    groups += [[255] * 16, [0] * 16]
    for pixels in groups:
        words = [pixels[2 * j] << 8 | pixels[2 * j + 1] for j in range(8)]
        machine = Machine(words)
        machine.run(lines)
        got = [machine.y.get(0x48 + k, 0) for k in range(8)]
        want = [plane << 8 for plane in reference(pixels)]
        if [g & 0xFFFF00 for g in got] != want:
            raise SystemExit(f"c2p check failed for {pixels}: got {got}, want {want}")
    return len(groups)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    lines = emit()
    groups = check(lines)
    body = [line for line in lines if not line.startswith(";")]
    header = [
        "; Generated by gen-c2p.py; do not edit. One 16-pixel group, chunky to",
        f"; planar, in {len(body)} instructions; checked against a direct c2p for",
        f"; {groups} groups.",
    ]
    args.output.write_text("\n".join(header + lines) + "\n")


if __name__ == "__main__":
    main()
