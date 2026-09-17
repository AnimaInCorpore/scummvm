#!/usr/bin/env python3
"""Run the FCM live diagnostic in the game and verify generated samples and DMA.

The Falcon reads only the FCM package. Host PCM is generated after it exits,
for an independent source checksum and the existing waveform/deadline gate.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
TRANSPORT = HERE.parent / "foa-faithful-music"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--analyze-only", action="store_true")
    args, runner_args = parser.parse_known_args()
    # Check analysis dependencies before spending time on an emulator run.
    spec = importlib.util.spec_from_file_location("transport_analysis", TRANSPORT / "analyze-in-game.py")
    analyzer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(analyzer)
    if not args.analyze_only:
        subprocess.run([sys.executable, str(TRANSPORT / "in-game-gate.py"),
                        str(args.package.resolve()), "--live-fcm", "--output", str(args.output.resolve()),
                        *runner_args], check=True)
    stats = json.loads((args.output / "result.json").read_text())
    if stats.get("live_synthesis") != 1 or stats["read_bytes"] or stats["ring_bytes"]:
        raise ValueError("Run did not select the live renderer without PCM reads")
    if stats["source_sha256"] != hashlib.sha256(args.package.read_bytes()).hexdigest():
        raise ValueError("Reference package differs from the package run on the Falcon")
    source = args.output.resolve() / "live-reference.raw"
    # Re-analysis independently regenerates the source, preserving an existing
    # identical reference and refusing to replace a different one.
    with tempfile.TemporaryDirectory(dir=args.output) as temporary:
        generated = Path(temporary) / "source.raw"
        host = json.loads(subprocess.check_output([str(HERE / "build/live-demo"), str(args.package.resolve()),
                        str(generated), str(stats["consumed_bytes"] // 4)], text=True))
        if source.exists():
            if source.read_bytes() != generated.read_bytes():
                raise ValueError("Existing PCM reference differs from the current host renderer")
        else:
            generated.rename(source)
    exact = (host["native_frames"] == stats["live_native_frames"] and
             host["native_checksum"] == stats["live_native_checksum"])
    result = analyzer.analyze(args.output, source)
    result.update({"live_native_check_passed": exact, "host_native": host,
                   "waveform_reference_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                   "synthesis": "One live factory Xylophone partial at 32 kHz; dry, scripted notes",
                   "full_mt32_synthesis": False, "reverb": False, "live_imuse_routing": False})
    result["passed"] = result["passed"] and exact
    with (args.output / "live-analysis.json").open("x") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps({k: v for k, v in result.items() if k != "waveform_windows"}, indent=2))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
