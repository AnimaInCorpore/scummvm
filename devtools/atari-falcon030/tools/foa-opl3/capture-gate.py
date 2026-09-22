#!/usr/bin/env python3
"""Capture the Atlantis AdLib register stream twice and summarize one run.

The gate requires the two runs to be byte identical, so the reported numbers
belong to a reproducible register stream rather than to one scheduling
accident. It measures register traffic and keyed-voice occupancy only: no
audio is rendered, no emulator is reached and no renderer is timed.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

from gate_env import source_sha256

HERE = Path(__file__).resolve().parent
SOURCES = ("trace-opl.cpp", "opl.mk", "build-capture.sh", "capture-opl.py",
           "analyze-opl.py", "test-analyze-opl.py", "capture-gate.py")

_spec = importlib.util.spec_from_file_location("analyze_opl", HERE / "analyze-opl.py")
analyze_opl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(analyze_opl)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--milliseconds", type=int, default=60000)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)

    traces = {}
    for run in ("a", "b"):
        case = args.output / f"run-{run}"
        subprocess.run([sys.executable, str(HERE / "capture-opl.py"), "--game", str(args.game),
                        "--milliseconds", str(args.milliseconds), "--output", str(case)],
                       check=True, stdout=subprocess.DEVNULL)
        traces[run] = (case / "opl-writes.ev").read_bytes()
    if traces["a"] != traces["b"]:
        raise SystemExit("Repeated captures differ; inspect the retained runs")

    manifest = json.loads((args.output / "run-a/capture.json").read_text())
    analysis = analyze_opl.analyze(traces["a"].decode())
    (args.output / "analysis.json").write_text(json.dumps(analysis, indent=2) + "\n")

    repository = subprocess.run(["git", "-C", str(HERE), "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "-C", str(HERE), "status", "--porcelain"],
                                capture_output=True, text=True, check=True).stdout.strip())
    result = {
        "date": "2026-09-16",
        "gate": "Repeated virtual-clock Atlantis capture of post-AdLib-driver OPL writes",
        "scummvm_commit": repository,
        "scummvm_worktree_dirty": dirty,
        "source_sha256": {name: source_sha256(HERE / name) for name in SOURCES},
        "binary_sha256": manifest["binary_sha256"],
        "game_sha256": manifest["game_sha256"],
        "capture_ms": args.milliseconds,
        "runs": 2,
        "repeated_runs_byte_identical": True,
        "trace_sha256": sha(traces["a"]),
        "clock": manifest["clock"],
        "sound_starts": manifest["sound_starts"],
        "scripted_user_input": None,
        "speech_muted": True,
        "analysis": analysis,
        "not_established": [
            "No audio was rendered or compared against an OPL reference",
            "No DSP kernel exists, so no synthesis or transport cost was measured",
            "One unattended opening sequence only; no player input and no other scenes",
            "Sam & Max OPL3 data was not available, so its layered path is unmeasured",
        ],
    }
    (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "analysis"}, indent=2))


if __name__ == "__main__":
    main()
