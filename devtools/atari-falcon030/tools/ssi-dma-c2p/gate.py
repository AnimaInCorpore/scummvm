#!/usr/bin/env python3
"""Run SSIECHO.TOS, the SSI DMA pass-through test, on the emulated Falcon.

The program routes DMA playback through the DSP's SSI echo into DMA record
and reports, per crossbar clock, the frame rate and throughput over a 2 s
window and whether a 256,000-byte pattern comes back intact. Its own
verdict is strict, for a real Falcon: no slot missed, no word changed.

Hatari cannot meet that verdict, and this gate does not ask it to. Hatari
runs the DSP only between 68030 instructions, so an instruction that takes
longer than one SSI slot (2.0 us at 62,500 Hz frames, 2.5 us at 49,170 Hz)
delivers two words with no DSP cycles between them, and the echo loses the
first: the record then holds the word before it twice. Reads of the MFP,
the sound registers and bus-contended ST-RAM do that now and then; on a
Falcon the DSP runs beside the 68030 and never sees them. So the gate checks
the mechanics instead: the route carries data at the modelled frame rate,
the pattern is found at one offset, and every damaged word is that single
repeat, few enough to be the emulator's.

The SSIECHO.TXT a real Falcon writes is the result that counts; this
gate's copy is the reference to compare it with.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "foa-opl3"))
from gate_env import HATARI, TOS402 as TOS, source_sha256  # noqa: E402

PLAY_WORDS = 128000
RATE_TOLERANCE = 0.001             # the program's own window tolerance
HATARI_DROP_LIMIT = PLAY_WORDS // 1000

CLOCK_RE = re.compile(r"^clock (.+)$")
WINDOW_RE = re.compile(r"^  window: frames (\d+)  words (\d+)  slot errors (\d+)  in ([\d.]+) ms")
RATE_RE = re.compile(r"^  frame rate ([\d.]+) Hz \(expected ([\d.]+) Hz\)  throughput (\d+) B/s")
PASS_RE = re.compile(r"^  pass: record buffer full after ([\d.]+) ms \((\d+) B/s\), "
                     r"DSP words (\d+)  slot errors (\d+)")
PATTERN_RE = re.compile(r"^  pattern at word (\d+) \(slot phase (\d+)\), mismatched words (\d+)")
MISMATCH_RE = re.compile(r"^    word (\d+): want (\w+) (\w+) (\w+)  got (\w+) (\w+) (\w+)")


def parse(lines):
    clocks, current = [], None
    for line in lines:
        if match := CLOCK_RE.match(line):
            current = {"clock": match.group(1), "mismatch_samples": []}
            clocks.append(current)
        elif current is None:
            continue
        elif match := WINDOW_RE.match(line):
            current.update(window_frames=int(match.group(1)), window_words=int(match.group(2)),
                           window_slot_errors=int(match.group(3)),
                           window_ms=float(match.group(4)))
        elif match := RATE_RE.match(line):
            current.update(frame_rate_hz=float(match.group(1)),
                           expected_hz=float(match.group(2)),
                           bytes_per_second_each_way=int(match.group(3)))
        elif match := PASS_RE.match(line):
            current.update(pass_ms=float(match.group(1)), pass_bytes_per_second=int(match.group(2)),
                           pass_dsp_words=int(match.group(3)),
                           pass_slot_errors=int(match.group(4)))
        elif match := PATTERN_RE.match(line):
            current.update(pattern_offset=int(match.group(1)), slot_phase=int(match.group(2)),
                           mismatched_words=int(match.group(3)))
        elif match := MISMATCH_RE.match(line):
            want, got = match.groups()[1:4], match.groups()[4:7]
            current["mismatch_samples"].append({"word": int(match.group(1)),
                                                "want": list(want), "got": list(got)})
    return clocks


def hatari_verdict(clock):
    """Why the run is not the mechanics Hatari can show, or None."""
    for key in ("frame_rate_hz", "pattern_offset"):
        if key not in clock:
            return f"no {key.replace('_', ' ')} reported"
    if abs(clock["frame_rate_hz"] / clock["expected_hz"] - 1.0) > RATE_TOLERANCE:
        return "frame rate off the model"
    if clock["mismatched_words"] > HATARI_DROP_LIMIT:
        return f"{clock['mismatched_words']} damaged words, more than Hatari's scheduling explains"
    for sample in clock["mismatch_samples"]:
        # the emulator's signature: the word before repeated in place of this one
        if sample["got"][1] != sample["got"][0] or sample["got"][2] != sample["want"][2]:
            return f"word {sample['word']} is damaged in a way Hatari's scheduling does not explain"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vbls", type=int, default=3000)
    args = parser.parse_args()
    if not HATARI.is_file():
        parser.error(f"the DSP-calibrated Hatari is not at {HATARI}")
    program = HERE / "build/SSIECHO.TOS"
    if not program.is_file():
        parser.error("build/SSIECHO.TOS is missing; run build.sh first")
    args.output.mkdir(parents=True, exist_ok=False)
    shutil.copy(program, args.output)

    with (args.output / "debug.log").open("w") as log:
        subprocess.run([str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
                        "--conout", "2", "--tos", str(TOS), "--patch-tos", "true",
                        "--fast-boot", "true", "--fast-forward", "true", "--sound", "off",
                        "--confirm-quit", "false", "--run-vbls", str(args.vbls),
                        "--log-file", str((args.output / "hatari.log").resolve()), "SSIECHO.TOS"],
                       cwd=args.output, stdout=log, stderr=subprocess.STDOUT, check=True)

    report = args.output / "SSIECHO.TXT"
    if not report.is_file():
        raise SystemExit(f"the run wrote no SSIECHO.TXT; see {args.output}/debug.log")
    lines = report.read_text(errors="replace").splitlines()
    clocks = parse(lines)
    failures = {clock["clock"]: reason for clock in clocks
                if (reason := hatari_verdict(clock)) is not None}
    if len(clocks) != 2:
        failures["report"] = f"{len(clocks)} clocks reported, expected 2"
    result = {
        "gate": "SSI DMA pass-through on the emulated Falcon (Hatari: mechanics, not hardware)",
        "sources": {name: source_sha256(HERE / name)
                    for name in ("dsp/ssiecho.asm", "m68k/ssiecho.s", "gate.py")},
        "hardware_verdict": next((line for line in lines if line.startswith("RESULT:")), None),
        "clocks": clocks,
        "failures": failures,
        "pass": not failures,
    }
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("\n".join(lines))
    print()
    for clock in clocks:
        print(f"{clock['clock']}: {clock.get('bytes_per_second_each_way')} B/s each way, "
              f"{clock.get('mismatched_words')} of {PLAY_WORDS} words dropped by Hatari's scheduling")
    if failures:
        for name, reason in failures.items():
            print(f"GATE FAIL {name}: {reason}")
        raise SystemExit(1)
    print("GATE PASS: the route's mechanics hold under Hatari; the strict verdict needs a Falcon")


if __name__ == "__main__":
    main()
