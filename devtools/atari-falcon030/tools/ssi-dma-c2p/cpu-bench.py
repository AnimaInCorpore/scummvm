#!/usr/bin/env python3
"""Time the backend's 68030 c2p (CPUC2P.TOS) on the emulated Falcon.

The figure to compare the DSP route with: the whole 320x200x8 screen, the
dirty-rectangle variant at a few sizes, and a plain screen copy. The
conversion is checked against a direct c2p first.

Hatari's 68030 is not cycle-exact: it approximates instruction timing and
ST-RAM contention, so these are the emulator's milliseconds. A real Falcon's
CPUC2P.TXT is the figure that counts.
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

TIME_RE = re.compile(r"^(.+?)\s+(\d+\.\d+) ms  \((\d+) ns a pixel, (\d+) pixels\)$")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vbls", type=int, default=3000)
    args = parser.parse_args()
    if not HATARI.is_file():
        parser.error(f"the DSP-calibrated Hatari is not at {HATARI}")
    program = HERE / "build/CPUC2P.TOS"
    if not program.is_file():
        parser.error("build/CPUC2P.TOS is missing; run build.sh with the cross compiler")
    args.output.mkdir(parents=True, exist_ok=False)
    shutil.copy(program, args.output)
    with (args.output / "debug.log").open("w") as log:
        subprocess.run([str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
                        "--conout", "2", "--tos", str(TOS), "--patch-tos", "true",
                        "--fast-boot", "true", "--fast-forward", "true", "--sound", "off",
                        "--confirm-quit", "false", "--run-vbls", str(args.vbls),
                        "--log-file", str((args.output / "hatari.log").resolve()), "CPUC2P.TOS"],
                       cwd=args.output, stdout=log, stderr=subprocess.STDOUT, check=True)
    report = args.output / "CPUC2P.TXT"
    if not report.is_file():
        raise SystemExit(f"the run wrote no CPUC2P.TXT; see {args.output}/debug.log")
    lines = report.read_text(errors="replace").splitlines()
    cases = [{"case": m.group(1), "ms": float(m.group(2)), "ns_per_pixel": int(m.group(3)),
              "pixels": int(m.group(4))} for line in lines if (m := TIME_RE.match(line))]
    verdict = next((line for line in lines if line.startswith("RESULT:")), None)
    result = {
        "bench": "68030 c2p of the ScummVM Atari backend on the emulated Falcon (Hatari)",
        "sources": {name: source_sha256(HERE / name) for name in ("cpu/cpuc2p.c", "cpu-bench.py")},
        "c2p_source": source_sha256(HERE.parents[3] / "backends/graphics/atari/atari-c2p-asm.S"),
        "cases": cases,
        "verdict": verdict,
    }
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("\n".join(lines))
    if verdict != "RESULT: PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
