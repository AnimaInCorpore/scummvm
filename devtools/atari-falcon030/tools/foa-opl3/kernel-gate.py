#!/usr/bin/env python3
"""Run the host OPL kernel against Nuked-OPL3 and record the result.

Sample equality is the bar: Nuked is a bit-exact chip model, so any
difference is a defect, not a tolerance. This gate measures no timing and
runs nothing on a Falcon or a DSP.
"""
import argparse
import hashlib
from datetime import date
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SOURCES = ("opl-kernel.h", "kernel-test.cpp", "generate-tables.py", "kernel.mk", "kernel-gate.py")
BINARY = HERE / "build/headless/opl-kernel-test"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, required=True, help="captured opl-writes.ev to replay")
    parser.add_argument("--rhythm-trace", type=Path,
                        help="a second captured stream, of a game that uses rhythm mode (Cruise for a Corpse)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not BINARY.is_file():
        parser.error("build opl-kernel-test first (see README)")
    args.output.mkdir(parents=True, exist_ok=False)

    completed = subprocess.run([str(BINARY), str(args.trace.resolve())]
                               + ([str(args.rhythm_trace.resolve())] if args.rhythm_trace else []),
                               capture_output=True, text=True, check=True)
    summary = json.loads(completed.stdout.strip().splitlines()[-1])
    if not summary["bit_exact"] or summary["mismatches"]:
        raise SystemExit("kernel differs from the reference")

    repository = subprocess.run(["git", "-C", str(HERE), "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "-C", str(HERE), "status", "--porcelain"],
                                capture_output=True, text=True, check=True).stdout.strip())
    result = {
        "date": date.today().isoformat(),
        "gate": "DSP-shaped OPL kernel against Nuked-OPL3, sample for sample",
        "scummvm_commit": repository,
        "scummvm_worktree_dirty": dirty,
        "source_sha256": {name: sha((HERE / name).read_bytes()) for name in SOURCES},
        "trace_sha256": sha(args.trace.read_bytes()),
        "rhythm_trace_sha256": sha(args.rhythm_trace.read_bytes()) if args.rhythm_trace else None,
        "reference": summary["reference"],
        "samples_compared": summary["samples_compared"],
        "register_writes": summary["register_writes"],
        "mismatches": summary["mismatches"],
        "bit_exact": True,
        "cases": ["waveform range", "pitch: every multiplier, key-scale rate, block and f-number spread",
                  "envelope: every attack/decay pair and every sustain/release pair, both envelope types",
                  "level: every key-scale level against total levels and blocks",
                  "timbre: all waveforms, both connections, every feedback depth, OPL2 and OPL3",
                  "modulation: tremolo and vibrato, shallow and deep, over a full LFO period",
                  "polyphony: 9 and 18 channels with key cycling and mid-note patch reloads",
                  "rhythm: the bass drum in both connections and the four single-operator drums in every"
                  " key combination, waveform and level; drum keys under and over the channels' own; the"
                  " mode left and entered under sounding notes; the low bank of an 18-channel chip; the"
                  " noise generator past its period",
                  "waveform select enable: an OPL2's gate, against the oracle given what it lets through",
                  "the captured Atlantis register stream replayed at its recorded times"]
                 + (["the captured Cruise for a Corpse register stream, which plays its percussion through"
                     " rhythm mode, replayed at its recorded times"] if args.rhythm_trace else []),
        "rom_words": {"log_sine": 256, "exponential": 256,
                      "note": "The eight 1,024-entry waveform tables a desktop build uses are"
                              " reconstructed from these two, so the whole ROM is 512 words"},
        "scope": {"two_operator_melodic_channels": True, "four_operator_mode": False,
                  "rhythm_mode": True, "opl2_waveform_select_enable": True},
        "not_established": [
            "No DSP assembly was measured; this is the host reference the kernel is written from",
            "No synthesis, transport or deadline cost was timed on a Falcon or in emulation",
            "Equality holds at the chip's native 49,716 Hz; no output-rate conversion is modelled",
        ],
    }
    (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
