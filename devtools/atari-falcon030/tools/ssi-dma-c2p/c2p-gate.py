#!/usr/bin/env python3
"""Run SSIC2P.TOS, the DSP c2p over the SSI DMA route, on the emulated Falcon.

The program streams one random 320x200 8-bit chunky screen through the DSP
per pass, two passes per crossbar clock, and checks the planar words DMA
record wrote against a 68030 reference c2p. This gate runs it under Hatari
and requires the program's own verdict: every planar word right, at the same
offset in both passes, with the DSP's ring never near overrun.

Unlike the pass-through gate, it asks Hatari for an exact result. Hatari
runs the DSP only between 68030 instructions, and one that outlasts an SSI
slot (a DBF costs it up to 42 cycles, a slot is 40 or 32) can hand the DSP
two words with no cycles between them; here a lost word would shift every
later group. So a pass STOPs the 68030 until record's end interrupt, and a
stopped 68030 advances in small steps. The DSP's backlog is Hatari's cycle
model, not the hardware; a real Falcon's SSIC2P.TXT is the result that
counts for timing.
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

CLOCK_RE = re.compile(r"^clock (.+)$")
PASS_RE = re.compile(r"^  pass (\d+): groups (\d+)  backlog (\d+) words  overruns (\d+)  state (\d+)")
PLANAR_RE = re.compile(r"^    planar at word (\d+) \(slot phase (\d+)\), mismatched words (\d+)")


def parse(lines):
    clocks, current, run = [], None, None
    for line in lines:
        if match := CLOCK_RE.match(line):
            current = {"clock": match.group(1), "passes": []}
            clocks.append(current)
        elif current is None:
            continue
        elif match := PASS_RE.match(line):
            run = {"groups": int(match.group(2)), "backlog_words": int(match.group(3)),
                   "overruns": int(match.group(4)), "state": int(match.group(5)),
                   "verdict": line.rstrip().rsplit(None, 1)[-1]}
            current["passes"].append(run)
        elif run is not None and (match := PLANAR_RE.match(line)):
            run.update(offset_words=int(match.group(1)), slot_phase=int(match.group(2)),
                       mismatched_words=int(match.group(3)),
                       planar_verdict=line.rstrip().rsplit(None, 1)[-1])
    return clocks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vbls", type=int, default=3000)
    args = parser.parse_args()
    if not HATARI.is_file():
        parser.error(f"the DSP-calibrated Hatari is not at {HATARI}")
    program = HERE / "build/SSIC2P.TOS"
    if not program.is_file():
        parser.error("build/SSIC2P.TOS is missing; run build.sh first")
    args.output.mkdir(parents=True, exist_ok=False)
    shutil.copy(program, args.output)

    with (args.output / "debug.log").open("w") as log:
        subprocess.run([str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
                        "--conout", "2", "--tos", str(TOS), "--patch-tos", "true",
                        "--fast-boot", "true", "--fast-forward", "true", "--sound", "off",
                        "--confirm-quit", "false", "--run-vbls", str(args.vbls),
                        "--log-file", str((args.output / "hatari.log").resolve()), "SSIC2P.TOS"],
                       cwd=args.output, stdout=log, stderr=subprocess.STDOUT, check=True)

    report = args.output / "SSIC2P.TXT"
    if not report.is_file():
        raise SystemExit(f"the run wrote no SSIC2P.TXT; see {args.output}/debug.log")
    lines = report.read_text(errors="replace").splitlines()
    verdict = next((line for line in lines if line.startswith("RESULT:")), None)
    result = {
        "gate": "DSP c2p over the SSI DMA route on the emulated Falcon (Hatari)",
        "sources": {name: source_sha256(HERE / name)
                    for name in ("dsp/ssic2p.asm", "gen-c2p.py", "m68k/ssic2p.s", "m68k/common.i",
                                 "m68k/common.s", "c2p-gate.py")},
        "verdict": verdict,
        "clocks": parse(lines),
        "pass": verdict == "RESULT: PASS",
    }
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("\n".join(lines))
    if not result["pass"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
