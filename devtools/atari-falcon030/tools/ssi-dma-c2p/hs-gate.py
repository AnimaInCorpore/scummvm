#!/usr/bin/env python3
"""Run HSC2P.TOS, the DSP c2p in handshake mode, on the emulated Falcon.

The program streams one random 320x200 8-bit chunky screen through the DSP
per pass, three passes, the second with the 68030 busy on long
instructions throughout, and checks what DMA record wrote word for word
against the backend's planar line layout of a 68030 reference c2p. This
gate requires the program's own verdict: every word right in every pass.

In handshake mode the DSP asks for each word it takes and releases each
word it gives, so, unlike the free-running mode (see c2p-gate.py), the busy
pass must be as exact as the quiet ones under Hatari too. The pass times
are Hatari's; a real Falcon's HSC2P.TXT is the result that counts for them.
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

PASS_RE = re.compile(r"^  pass (\d+) \((\w+)\): (\d+) ms, chunky B/s (\d+)  groups (\d+)  state (\d+)")
RECORD_RE = re.compile(r"^    record: mismatched words (\d+)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vbls", type=int, default=3000)
    args = parser.parse_args()
    if not HATARI.is_file():
        parser.error(f"the DSP-calibrated Hatari is not at {HATARI}")
    program = HERE / "build/HSC2P.TOS"
    if not program.is_file():
        parser.error("build/HSC2P.TOS is missing; run build.sh first")
    args.output.mkdir(parents=True, exist_ok=False)
    shutil.copy(program, args.output)
    with (args.output / "debug.log").open("w") as log:
        subprocess.run([str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
                        "--conout", "2", "--tos", str(TOS), "--patch-tos", "true",
                        "--fast-boot", "true", "--fast-forward", "true", "--sound", "off",
                        "--confirm-quit", "false", "--run-vbls", str(args.vbls),
                        "--log-file", str((args.output / "hatari.log").resolve()), "HSC2P.TOS"],
                       cwd=args.output, stdout=log, stderr=subprocess.STDOUT, check=True)
    report = args.output / "HSC2P.TXT"
    if not report.is_file():
        raise SystemExit(f"the run wrote no HSC2P.TXT; see {args.output}/debug.log")
    lines = report.read_text(errors="replace").splitlines()
    passes, current = [], None
    for line in lines:
        if match := PASS_RE.match(line):
            current = {"pass": int(match.group(1)), "kind": match.group(2), "ms": int(match.group(3)),
                       "chunky_bytes_per_second": int(match.group(4)), "groups": int(match.group(5)),
                       "state": int(match.group(6))}
            passes.append(current)
        elif current is not None and (match := RECORD_RE.match(line)):
            current["mismatched_words"] = int(match.group(1))
    verdict = next((line for line in lines if line.startswith("RESULT:")), None)
    result = {
        "gate": "DSP c2p in handshake mode on the emulated Falcon (Hatari)",
        "sources": {name: source_sha256(HERE / name)
                    for name in ("dsp/hsc2p.asm", "gen-c2p.py", "m68k/hsc2p.s", "m68k/common.i",
                                 "m68k/common.s", "hs-gate.py")},
        "verdict": verdict,
        "passes": passes,
        "pass": verdict == "RESULT: PASS",
    }
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("\n".join(lines))
    if not result["pass"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
