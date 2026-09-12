#!/usr/bin/env python3
"""What Fate of Atlantis actually asks of an MT-32.

Reads the game's own MT-32 music out of ATLANTIS.001 and reports the
time-weighted distribution of simultaneously sounding notes, split into the
melodic parts (LA synth partials) and the rhythm part (PCM partials).

    python3 foa-mt32-demand.py [path to atlantis-cd]
    python3 foa-mt32-demand.py --export <dir> [path to atlantis-cd]

--export writes one <dir>/cue-NNN.ev per cue for the Munt harness in
tools/mt32-partials: each line is "<microseconds> <hex bytes>", absolute from
the start of the cue, tempo already resolved. Note counts here and partial
counts there are then measured over exactly the same event stream.

Default game directory: ../../../assets/atlantis-cd relative to this file,
else $FOA_DATA_DIR.

Method: ATLANTIS.001 is XOR-0x69 encrypted; inside it, SCUMM v5 'ROL ' blocks
carry an MDhd header followed by a standard MIDI file. Note counts are weighted
by real time, honouring the 368 distinct tempo changes in the set - the cues
run from 24 to 400 BPM, so tick-weighting is not time-weighting.
"""
import collections, os, pathlib, struct, sys

XOR = 0x69

def load(game_dir):
    p = pathlib.Path(game_dir) / "ATLANTIS.001"
    if not p.exists():
        sys.exit(f"no ATLANTIS.001 in {game_dir}")
    return bytes(b ^ XOR for b in p.read_bytes())

def find_rol_blocks(data):
    """Every 'ROL ' resource that contains an SMF. Size is big-endian, header included."""
    out, i = [], 0
    while True:
        i = data.find(b'ROL ', i)
        if i < 0:
            return out
        size = struct.unpack(">I", data[i + 4:i + 8])[0]
        if 8 < size < 2_000_000 and i + size <= len(data):
            body = data[i + 8:i + size]
            if b'MThd' in body[:64]:
                out.append(body)
        i += 4

def varlen(b, p):
    v = 0
    while True:
        c = b[p]
        p += 1
        v = (v << 7) | (c & 0x7F)
        if not c & 0x80:
            return v, p

def parse_track(trk):
    """(abs_tick, status, payload) for one MTrk, honouring running status."""
    p, t, run, n = 0, 0, None, len(trk)
    while p < n:
        try:
            dt, p = varlen(trk, p)
        except IndexError:
            return
        t += dt
        if p >= n:
            return
        b = trk[p]
        if b & 0x80:
            status = b
            p += 1
            if status < 0xF0:
                run = status
        else:
            status = run
            if status is None:
                return
        if status == 0xFF:
            if p >= n:
                return
            meta = trk[p]
            p += 1
            ln, p = varlen(trk, p)
            yield t, 0xFF, bytes([meta]) + trk[p:p + ln]
            p += ln
            if meta == 0x2F:
                return
        elif status in (0xF0, 0xF7):
            ln, p = varlen(trk, p)
            yield t, status, trk[p:p + ln]
            p += ln
        else:
            ln = 1 if (status & 0xF0) in (0xC0, 0xD0) else 2
            yield t, status, trk[p:p + ln]
            p += ln

def cue_events(body):
    """Tempo and note events of one cue, ordered, tempo first within a tick."""
    mthd = body.find(b'MThd')
    division = struct.unpack(">H", body[mthd + 12:mthd + 14])[0]
    p, evs = mthd + 14, []
    while True:
        mt = body.find(b'MTrk', p)
        if mt < 0:
            break
        ln = struct.unpack(">I", body[mt + 4:mt + 8])[0]
        for t, status, d in parse_track(body[mt + 8:mt + 8 + ln]):
            if status == 0xFF:
                if d and d[0] == 0x51 and len(d) >= 4:
                    evs.append((t, 2, (d[1] << 16) | (d[2] << 8) | d[3], 0))
            elif status in (0xF0, 0xF7):
                continue
            else:
                hi, ch = status & 0xF0, status & 0x0F
                if hi == 0x90 and len(d) > 1 and d[1] > 0:
                    evs.append((t, +1, ch, d[0]))
                elif hi == 0x80 or (hi == 0x90 and len(d) > 1 and d[1] == 0):
                    evs.append((t, -1, ch, d[0]))
        p = mt + 8 + ln
    evs.sort(key=lambda e: (e[0], 0 if e[1] == 2 else 1))
    return division, evs

def measure(data):
    total = collections.Counter()   # all sounding notes -> seconds
    melodic = collections.Counter()  # parts 1-8 -> seconds
    rhythm = collections.Counter()   # part 10 -> seconds
    parts = collections.Counter()    # distinct active parts -> seconds
    programs, cues, seconds = collections.Counter(), 0, 0.0
    for body in find_rol_blocks(data):
        cues += 1
        division, evs = cue_events(body)
        prev, us, active = None, 500_000, collections.Counter()
        for t, kind, a, b in evs:
            if prev is not None and t > prev:
                dt = (t - prev) * us / division / 1e6
                live = [(c, n) for (c, n), v in active.items() if v > 0]
                total[sum(active.values())] += dt
                melodic[sum(v for (c, n), v in active.items() if c != 9)] += dt
                rhythm[sum(v for (c, n), v in active.items() if c == 9)] += dt
                parts[len({c for c, n in live})] += dt
                seconds += dt
            prev = t
            if kind == 2:
                us = a
            elif kind > 0:
                active[(a, b)] += 1
            elif active[(a, b)] > 0:
                active[(a, b)] -= 1
    return cues, seconds, total, melodic, rhythm, parts

def decode_imuse_nibbles(src):
    """iMUSE packs sysex payloads two bytes per byte; Player::decode_sysex_bytes."""
    out = bytearray()
    for i in range(0, len(src) - 1, 2):
        out.append(((src[i] << 4) & 0xFF) | (src[i + 1] & 0x0F))
    return bytes(out)


def export(data, out_dir):
    """One .ev file per cue: absolute microseconds and the raw MIDI bytes."""
    out = pathlib.Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.glob("cue-*.ev"):
        stale.unlink()
    cues = 0
    for idx, body in enumerate(find_rol_blocks(data)):
        mthd = body.find(b'MThd')
        division = struct.unpack(">H", body[mthd + 12:mthd + 14])[0]
        p, raw = mthd + 14, []
        while True:
            mt = body.find(b'MTrk', p)
            if mt < 0:
                break
            ln = struct.unpack(">I", body[mt + 4:mt + 8])[0]
            for t, status, d in parse_track(body[mt + 8:mt + 8 + ln]):
                raw.append((t, status, bytes(d)))
            p = mt + 8 + ln
        # tempo first within a tick, so a change applies to the notes at it
        raw.sort(key=lambda e: (e[0], 0 if (e[1] == 0xFF and e[2][:1] == b'\x51') else 1))
        lines, us_per_qn, prev_tick, now_us = [], 500_000, 0, 0.0
        for t, status, d in raw:
            now_us += (t - prev_tick) * us_per_qn / division
            prev_tick = t
            if status == 0xFF:
                if d and d[0] == 0x51 and len(d) >= 4:
                    us_per_qn = (d[1] << 16) | (d[2] << 8) | d[3]
                continue
            if status == 0xF0:
                # iMUSE's own 0x7D messages are addressed to the sequencer, not to
                # the MT-32, and Munt rightly ignores them. But part-allocation
                # (code 0) carries the part's program, and the real driver forwards
                # it as a program change: see Player::decode_sysex_bytes and
                # part->_instrument.program(buf[8], ...) in sysex_scumm.cpp. Without
                # this, 3.58% of melodic note-ons play on whatever patch the reset
                # left behind.
                if d[:1] == b'\x7d' and len(d) > 2 and d[1] == 0x00:
                    ch = d[2] & 0x0F
                    buf = decode_imuse_nibbles(d[3:])
                    if len(buf) >= 8 and not (buf[4] & 0x80) and buf[7] < 128:
                        lines.append(f"{int(now_us)} {bytes([0xC0 | ch, buf[7]]).hex()}")
                # Pass the message on anyway: framed, so Munt can classify it.
                payload = b'\xf0' + d
                if not payload.endswith(b'\xf7'):
                    payload += b'\xf7'
                lines.append(f"{int(now_us)} {payload.hex()}")
            elif status == 0xF7:
                continue
            else:
                lines.append(f"{int(now_us)} {bytes([status]).hex()}{d.hex()}")
        if lines:
            (out / f"cue-{idx:03d}.ev").write_text("\n".join(lines) + "\n")
            cues += 1
    print(f"exported {cues} cues to {out}")


def main():
    here = pathlib.Path(__file__).resolve().parent
    default = here / ".." / ".." / ".." / "assets" / "atlantis-cd"
    argv = sys.argv[1:]
    out_dir = None
    if argv and argv[0] == "--export":
        if len(argv) < 2:
            sys.exit("--export needs a directory")
        out_dir, argv = argv[1], argv[2:]
    game_dir = argv[0] if argv else os.environ.get("FOA_DATA_DIR", default)
    data = load(game_dir)
    if out_dir is not None:
        export(data, out_dir)
        return
    cues, secs, total, melodic, rhythm, parts = measure(data)

    print(f"{cues} MT-32 cues, {secs / 60:.1f} min of music "
          f"({(secs - total[0]) / 60:.1f} min sounding)\n")

    def table(name, hist, limit):
        print(f"{name}: time-weighted concurrent notes")
        cum = 0.0
        for k in sorted(hist):
            if k > limit:
                continue
            pct = 100 * hist[k] / secs
            cum += pct
            print(f"  {k:2d} : {pct:5.1f}%  cum {cum:5.1f}%")
        print()

    table("All parts", total, 12)
    table("Melodic parts 1-8 (synth partials)", melodic, 12)

    active = secs - rhythm[0]
    print(f"Rhythm part 10 (PCM partials): active {100 * active / secs:.1f}% of the time, "
          f"mean {sum(k * v for k, v in rhythm.items()) / max(1e-9, active):.2f} notes when active\n")

    print("Simultaneously active parts (of 9):")
    for k in sorted(parts):
        print(f"  {k} : {100 * parts[k] / secs:5.1f}%")

    def cov(n):
        return 100 * sum(v for k, v in melodic.items() if k <= n) / secs

    print("\nShare of playing time that fits a partial budget, by partials-per-note:")
    print("  partials ->      1       2       3       4")
    for budget in (4, 5, 7, 8, 32):
        cells = " ".join(f"{cov(budget // ppn):6.1f}%" for ppn in (1, 2, 3, 4))
        print(f"  {budget:2d} partials  {cells}")

if __name__ == "__main__":
    main()
