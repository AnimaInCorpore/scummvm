#!/usr/bin/env python3
"""Can the DAC play one slot pair while the others carry data? SSIMIX.TOS
under Hatari, with the DAC's output recorded.

SSIMIX.TOS sends a tone in one slot pair of the four-track crossbar frame
and tags in the other slots, and points the DAC at a pair with
Setmontracks; it checks itself, in a record snapshot, which slot every word
landed in. What the DAC played it cannot hear. This gate records Hatari's
sound (an AVI's sound track, as foa-opl3/game-gate.py does), finds the
cases between the silences the program keeps around them, and measures in
each how much of each channel's energy is the expected tone: 1,024.375 Hz
on the left, 2,048.75 Hz on the right (a 48-entry sine at 49,170 frames a
second, one and two steps a frame), as the median over 100 ms windows.

The cases that route the tone's pair to the DAC must play both tones and
little else; the control, whose DAC track carries tags, must not. The fifth
case, the layout a DSP c2p with sound would use (tone in pair 3 for DAC
track 3, three record tracks), is hardware only: its figures are recorded
here and not judged.

This is Hatari's crossbar, not the hardware's. Hatari lets the DAC pick its
track's pair out of the DSP transmit stream by slot position, as the Falcon
documentation describes for DMA playback, but it records every slot whatever
the record track count says, and decodes only DAC tracks 0 and 1 (its mask
for the monitor bits is 30 where 0x30 is meant). A real Falcon has to show
the rest.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys
import wave

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "foa-opl3"))
from gate_env import HATARI, TOS402 as TOS, source_sha256  # noqa: E402

_spec = importlib.util.spec_from_file_location("game_gate", HERE.parent / "foa-opl3/game-gate.py")
_game_gate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_game_gate)
write_avi_sound = _game_gate.write_avi_sound

FRAME_RATE = 49169.921875
LEFT_HZ, RIGHT_HZ = FRAME_RATE / 48, 2 * FRAME_RATE / 48
MEASURE = (0.3, 1.7)              # seconds into a case
WINDOW_S = 0.1                    # measured in windows of this length
SILENCE_GAP_S = 0.3               # the program keeps 0.5 s of zeros between cases
CASE_MIN_S = 1.5                  # a case plays 2.2 s
TONE_SHARE = 0.8                  # median share of a channel's AC energy
CONTROL_SHARE = 0.1

CASE_RE = re.compile(r"^case (\d) (.+)$")


def goertzel_power(samples, rate, freq):
    omega = 2 * math.pi * freq / rate
    coeff = 2 * math.cos(omega)
    s1 = s2 = 0.0
    for x in samples:
        s1, s2 = x + coeff * s1 - s2, s1
    return (s1 * s1 + s2 * s2 - coeff * s1 * s2) / len(samples) ** 2 * 2


def tone_share(samples, rate, freq):
    """The tone's share of the window's energy about its mean."""
    mean = sum(samples) / len(samples)
    ac = [x - mean for x in samples]
    energy = sum(x * x for x in ac) / len(ac)
    if energy < 1.0:
        return 0.0, 0.0
    return goertzel_power(ac, rate, freq) / energy, math.sqrt(energy)


def windowed_share(samples, rate, freq):
    """Median and lowest tone share over WINDOW_S windows, and the RMS.
    A slot the DSP misses under Hatari glitches a window; the median shows
    the route, the lowest the glitches."""
    step = round(WINDOW_S * rate)
    shares, levels = [], []
    for start in range(0, len(samples) - step + 1, step):
        share, level = tone_share(samples[start:start + step], rate, freq)
        shares.append(share)
        levels.append(level)
    shares.sort()
    levels.sort()
    return shares[len(shares) // 2], shares[0], levels[len(levels) // 2]


def case_spans(left, right, rate):
    """Where the cases play: the stretches between the program's silences.
    The DSP sends exact zeros between cases and the DAC plays them as such;
    a case, even the control, never does for SILENCE_GAP_S. Stretches
    shorter than CASE_MIN_S (clicks while routes change) are dropped."""
    gap = round(SILENCE_GAP_S * rate)
    spans, start, zeros = [], None, 0
    for i in range(len(left)):
        if left[i] or right[i]:
            if start is None:
                start = i
            zeros = 0
        elif start is not None:
            zeros += 1
            if zeros >= gap:
                spans.append((start, i - zeros + 1))
                start, zeros = None, 0
    if start is not None:
        spans.append((start, len(left) - zeros))
    return [span for span in spans if span[1] - span[0] >= CASE_MIN_S * rate]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vbls", type=int, default=1500)
    args = parser.parse_args()
    if not HATARI.is_file():
        parser.error(f"the DSP-calibrated Hatari is not at {HATARI}")
    program = HERE / "build/SSIMIX.TOS"
    if not program.is_file():
        parser.error("build/SSIMIX.TOS is missing; run build.sh first")
    args.output.mkdir(parents=True, exist_ok=False)
    shutil.copy(program, args.output)
    avi = (args.output / "output.avi").resolve()
    env = dict(__import__("os").environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    with (args.output / "debug.log").open("w") as log:
        subprocess.run([str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
                        "--conout", "2", "--tos", str(TOS), "--patch-tos", "true",
                        "--fast-boot", "true", "--fast-forward", "true", "--sound", "48000",
                        "--avirecord", "on", "--avi-vcodec", "png", "--avi-file", str(avi),
                        "--confirm-quit", "false", "--run-vbls", str(args.vbls),
                        "--log-file", str((args.output / "hatari.log").resolve()), "SSIMIX.TOS"],
                       cwd=args.output, stdout=log, stderr=subprocess.STDOUT, check=True, env=env)

    report = args.output / "SSIMIX.TXT"
    if not report.is_file():
        raise SystemExit(f"the run wrote no SSIMIX.TXT; see {args.output}/debug.log")
    lines = report.read_text(errors="replace").splitlines()
    wav = args.output / "output.wav"
    if not write_avi_sound(avi, wav):
        raise SystemExit("the AVI holds no sound")
    avi.unlink()
    with wave.open(str(wav), "rb") as w:
        rate, channels, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if channels != 2 or width != 2:
        raise SystemExit(f"unexpected sound format: {channels} channels, {width} bytes")
    count = len(raw) // 4
    left = [int.from_bytes(raw[4 * i:4 * i + 2], "little", signed=True) for i in range(count)]
    right = [int.from_bytes(raw[4 * i + 2:4 * i + 4], "little", signed=True) for i in range(count)]

    cases, current = [], None
    for line in lines:
        if match := CASE_RE.match(line):
            current = {"case": int(match.group(1)), "name": match.group(2)}
            cases.append(current)
        elif current is not None and line.startswith("  record slot:"):
            current["record"] = line.strip()
    if len(cases) != 5:
        raise SystemExit("the report does not name five cases")

    spans = case_spans(left, right, rate)
    failures = []
    if len(spans) != len(cases):
        failures.append(f"{len(spans)} sounding stretches in the recording, expected {len(cases)}")
    for case, (start, end) in zip(cases, spans):
        case["seconds"] = [round(start / rate, 3), round(end / rate, 3)]
        a, b = start + round(MEASURE[0] * rate), start + round(MEASURE[1] * rate)
        if b > end:
            failures.append(f"case {case['case']}: its stretch is too short")
            continue
        case["left_share"], case["left_lowest"], case["left_rms"] = windowed_share(left[a:b], rate, LEFT_HZ)
        case["right_share"], case["right_lowest"], case["right_rms"] = windowed_share(right[a:b], rate, RIGHT_HZ)
        control = "control" in case["name"]
        if "hardware only" in case["name"]:
            case["dac_verdict"] = "not judged (hardware only)"
            continue
        if control:
            ok = case["left_share"] < CONTROL_SHARE and case["right_share"] < CONTROL_SHARE
        else:
            ok = case["left_share"] > TONE_SHARE and case["right_share"] > TONE_SHARE
        case["dac_verdict"] = "PASS" if ok else "FAIL"
        if not ok:
            failures.append(f"case {case['case']}: tone shares {case['left_share']:.3f}/{case['right_share']:.3f}")

    verdict = next((line for line in lines if line.startswith("RESULT:")), None)
    if verdict != "RESULT: PASS":
        failures.append(f"the program's record checks: {verdict}")
    result = {
        "gate": "DAC on one slot pair of the four-track frame, under Hatari",
        "sources": {name: source_sha256(HERE / name)
                    for name in ("dsp/ssimix.asm", "m68k/ssimix.s", "mix-gate.py")},
        "sound_rate": rate,
        "tones_hz": [LEFT_HZ, RIGHT_HZ],
        "cases": cases,
        "failures": failures,
        "pass": not failures,
    }
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("\n".join(lines))
    print()
    for case in cases:
        if "left_share" in case:
            print(f"DAC case {case['case']}: left {LEFT_HZ:.1f} Hz share {case['left_share']:.3f} "
                  f"(lowest {case['left_lowest']:.3f}, rms {case['left_rms']:.0f}), "
                  f"right {RIGHT_HZ:.1f} Hz share {case['right_share']:.3f} "
                  f"(lowest {case['right_lowest']:.3f}, rms {case['right_rms']:.0f})  {case['dac_verdict']}")
    if failures:
        for failure in failures:
            print("GATE FAIL", failure)
        raise SystemExit(1)
    print("GATE PASS")


if __name__ == "__main__":
    main()
