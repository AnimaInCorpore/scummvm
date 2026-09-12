#!/usr/bin/env python3
"""How much of Fate of Atlantis' partial demand reads PCM waves.

The Falcon port splits the MT-32's partial pool across two processors: LA synth
partials on the DSP56001 and PCM partials on the 68030, with separate and
non-interchangeable budgets. This reports what share of the game's melodic
partial demand is PCM, which decides whether the PCM ROM obstacle is on the
critical path for the rhythm part alone or for most of the music.

    python3 foa-mt32-pcm.py <control-rom> [path to atlantis-cd]

Needs an MT-32 control ROM v1.07 (the timbre tables live in it). Nothing is
redistributable; supply your own dump.

Method: the control ROM's group A and B timbre maps give 64 timbre addresses
each; a timbre's common area carries partialMute (which of its four partials
sound) and the two partial-structure values. Munt's PartialStruct table
(Part.cpp) turns a structure into which partial of a pair reads PCM. Weighted
by the note-seconds each program accounts for in the game's own music.

Cross-validated against mt32-partials --probe, which measures partial counts by
playing a note: 127 of 128 programs agree on both count and name.
"""
import collections
import pathlib
import struct
import sys

# Imported by path because of the hyphens in the sibling filename.
_spec_path = pathlib.Path(__file__).resolve().parent / "foa-mt32-demand.py"
_ns = {}
exec(compile(_spec_path.read_text(), str(_spec_path), "exec"), _ns)
load, find_rol_blocks = _ns["load"], _ns["find_rol_blocks"]
parse_track, decode_imuse_nibbles = _ns["parse_track"], _ns["decode_imuse_nibbles"]

# ControlROMMap for "ctrl_mt32_1_07" (Munt, Synth.cpp)
TIMBRE_A_MAP, TIMBRE_A_OFFSET = 0x8000, 0x0000
TIMBRE_B_MAP, TIMBRE_B_OFFSET = 0xC000, 0x4000
TIMBRE_SIZE = 14 + 4 * 58  # sizeof(TimbreParam): common + four partials
# Munt, Part.cpp: bit 1 means the pair's first partial reads PCM, bit 0 the second
PARTIAL_STRUCT = (0, 0, 2, 2, 1, 3, 3, 0, 3, 0, 2, 1, 3)


def read_timbre(rom, group, index):
    base, offset = (TIMBRE_A_MAP, TIMBRE_A_OFFSET) if group == 0 else (TIMBRE_B_MAP, TIMBRE_B_OFFSET)
    entry = base + index * 2
    addr = ((rom[entry + 1] << 8) | rom[entry]) + offset
    t = rom[addr:addr + TIMBRE_SIZE]
    if len(t) < 14:
        raise ValueError(f"timbre {group}/{index} at 0x{addr:04x} is past the end of the ROM")
    return t[0:10].decode("latin1").rstrip(), t[10], t[11], t[12]


def program_costs(rom):
    """program -> (sounding partials, of which PCM, name). Patch defaults map
    program N to group A/B timbre N & 63, which the probe cross-check confirms."""
    out = {}
    for program in range(128):
        name, s12, s34, mute = read_timbre(rom, 0 if program < 64 else 1, program & 63)
        sounding = [bool(mute >> i & 1) for i in range(4)]
        p12, p34 = PARTIAL_STRUCT[s12], PARTIAL_STRUCT[s34]
        is_pcm = (bool(p12 & 2), bool(p12 & 1), bool(p34 & 2), bool(p34 & 1))
        out[program] = (sum(sounding),
                        sum(1 for i in range(4) if sounding[i] and is_pcm[i]),
                        name)
    return out


def note_seconds_per_program(data):
    """Melodic note-seconds attributed to the program sounding on each part.

    Programs arrive both as plain 0xC0 messages and inside iMUSE's
    part-allocation sysex, which the real driver forwards as a program change;
    both are honoured here, as in foa-mt32-demand.py --export.
    """
    ns = collections.Counter()
    unattributed = 0.0
    for body in find_rol_blocks(data):
        mthd = body.find(b"MThd")
        division = struct.unpack(">H", body[mthd + 12:mthd + 14])[0]
        pos, evs = mthd + 14, []
        while True:
            mt = body.find(b"MTrk", pos)
            if mt < 0:
                break
            ln = struct.unpack(">I", body[mt + 4:mt + 8])[0]
            for t, status, d in parse_track(body[mt + 8:mt + 8 + ln]):
                evs.append((t, status, bytes(d)))
            pos = mt + 8 + ln
        # Tempo first within a tick, so a change applies to the notes at it.
        evs.sort(key=lambda e: (e[0], 0 if (e[1] == 0xFF and e[2][:1] == b"\x51") else 1))

        program, active, prev, us_per_qn = {}, collections.Counter(), None, 500_000
        for t, status, d in evs:
            if prev is not None and t > prev:
                dt = (t - prev) * us_per_qn / division / 1e6
                for (ch, key), n in active.items():
                    if n > 0 and ch != 9:
                        if ch in program:
                            ns[program[ch]] += n * dt
                        else:
                            unattributed += n * dt
            prev = t
            if status == 0xFF:
                if d and d[0] == 0x51 and len(d) >= 4:
                    us_per_qn = (d[1] << 16) | (d[2] << 8) | d[3]
                continue
            if status in (0xF0, 0xF7):
                if d[:1] == b"\x7d" and len(d) > 2 and d[1] == 0x00:
                    ch = d[2] & 0x0F
                    buf = decode_imuse_nibbles(d[3:])
                    if len(buf) >= 8 and not (buf[4] & 0x80) and buf[7] < 128:
                        program[ch] = buf[7]
                continue
            hi, ch = status & 0xF0, status & 0x0F
            if hi == 0xC0:
                program[ch] = d[0]
            elif hi == 0x90 and len(d) > 1 and d[1] > 0:
                active[(ch, d[0])] += 1
            elif hi == 0x80 or (hi == 0x90 and len(d) > 1 and d[1] == 0):
                if active[(ch, d[0])] > 0:
                    active[(ch, d[0])] -= 1
    return ns, unattributed


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    rom = pathlib.Path(sys.argv[1]).read_bytes()
    if len(rom) != 65536:
        sys.exit(f"expected a 65536-byte MT-32 v1.x control ROM, got {len(rom)}")
    here = pathlib.Path(__file__).resolve().parent
    game_dir = sys.argv[2] if len(sys.argv) > 2 else here / ".." / ".." / ".." / "assets" / "atlantis-cd"

    costs = program_costs(rom)
    hist = collections.Counter((v[0], v[1]) for v in costs.values())
    print("All 128 programs, (sounding partials, of which PCM):")
    for key in sorted(hist):
        print(f"  {key[0]} partials, {key[1]} PCM : {hist[key]:3d} programs")
    print(f"  programs using at least one PCM partial: "
          f"{sum(1 for v in costs.values() if v[1] > 0)} of 128\n")

    ns, unattributed = note_seconds_per_program(load(game_dir))
    known = {p: v for p, v in ns.items() if p in costs}
    total = sum(known.values())
    if not total:
        sys.exit("no melodic note-seconds could be attributed to a program")
    partials = sum(costs[p][0] * v for p, v in known.items())
    pcm = sum(costs[p][1] * v for p, v in known.items())
    on_pcm_timbre = sum(v for p, v in known.items() if costs[p][1] > 0)

    print(f"Weighted by this game's {total / 60:.0f} minutes of melodic note-seconds:")
    print(f"  partials per melodic note:     {partials / total:.2f}")
    print(f"  PCM partials per melodic note: {pcm / total:.2f}")
    print(f"  PCM share of melodic partial demand: {100 * pcm / partials:.1f}%")
    print(f"  note-seconds on a timbre using PCM:  {100 * on_pcm_timbre / total:.1f}%")
    print(f"  (note-seconds on a part with no program set: "
          f"{100 * unattributed / (total + unattributed):.2f}%, excluded)\n")

    print("Heaviest timbres by share of melodic note-seconds:")
    for p, v in sorted(known.items(), key=lambda kv: -kv[1])[:10]:
        n, npcm, name = costs[p]
        print(f"  program {p:3d}  {name:<11} {n} partials, {npcm} PCM   {100 * v / total:5.1f}%")


if __name__ == "__main__":
    main()
