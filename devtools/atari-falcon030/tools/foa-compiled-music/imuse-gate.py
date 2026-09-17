#!/usr/bin/env python3
"""Compare actual post-iMUSE output with SMF/FCM in a virtual-clock game run.

No audio is rendered, no physical MIDI device is used, no real-time Falcon
performance is measured. Both variants use the real game and iMUSE engine.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import fcm

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--milliseconds", type=int, default=60000)
    args = parser.parse_args()
    if not 1000 <= args.milliseconds <= 600000:
        parser.error("duration must be 1–600 seconds")
    game, package, binary = args.game.resolve(), args.package.resolve(), args.binary.resolve()
    if any("\n" in str(p) or "\r" in str(p) for p in (game, package, binary)):
        parser.error("newline in path")
    if not binary.is_file() or not package.is_file():
        parser.error("build the comparison executable and FCM package first")
    args.output.mkdir(parents=True, exist_ok=False)
    traces, loads, fcm_loads = {}, {}, {}
    for variant in ("smf-a", "fcm-a", "smf-b", "fcm-b"):
        case = (args.output / variant).resolve()
        case.mkdir()
        setting = f"foa_fcm_score={package}\n" if variant.startswith("fcm") else ""
        (case / "scummvm.ini").write_text(
            "[scummvm]\nmusic_driver=null\nnative_mt32=true\n"
            f"foa_capture_ms={args.milliseconds}\n" + setting +
            "confirm_exit=false\nautosave_period=0\nsubtitles=true\nspeech_mute=true\n"
            "[foa-reference]\nengineid=scumm\ngameid=atlantis\n"
            f"path={game}\nplatform=pc\nlanguage=en\n")
        with (case / "scummvm.log").open("x") as log:
            subprocess.run([binary, "-c", "scummvm.ini", "--debugflags=imuse", "foa-reference"],
                           cwd=case, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        trace = (case / "imuse-midi.ev").read_bytes()
        fcm.require(trace.endswith(b"# end\n"), "incomplete MIDI trace")
        traces[variant] = trace
        log_text = (case / "scummvm.log").read_text()
        fcm.require("FCM1 test clock: virtual" in log_text, "not the virtual-clock comparison executable")
        loads[variant] = [line for line in log_text.splitlines() if "Starting music" in line]
        fcm_loads[variant] = [int(x) for x in re.findall(r"FCM1: cue (\d+),", log_text)]
        expected = [int(x) for x in re.findall(r"Starting music (\d+)", log_text)] if variant.startswith("fcm") else []
        fcm.require(fcm_loads[variant] == expected, "FCM parser selection not demonstrated")
    fcm.require(len(set(traces.values())) == 1, "SMF/FCM traces or repeated runs differ; inspect retained outputs")
    fcm.require(len({tuple(x) for x in loads.values()}) == 1, "sound starts differ")
    events = [line.split() for line in traces["smf-a"].decode().splitlines() if line and not line.startswith("#")]
    result = {"gate": "post-iMUSE virtual-clock opening, two runs of each parser", "passed": True,
              "events": len(events), "sysex": sum(e[1].startswith("f0") for e in events),
              "capture_ms": args.milliseconds, "trace_sha256": fcm.sha(traces["smf-a"]),
              "binary_sha256": fcm.sha(binary.read_bytes()), "package_sha256": fcm.sha(package.read_bytes()),
              "sound_starts": loads["smf-a"], "scripted_user_input": None, "speech_muted": True,
              "fcm_parser_cues": fcm_loads["fcm-a"],
              "audio_rendered": False, "falcon_performance_tested": False, "save_load_tested": False}
    (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))

if __name__ == "__main__":
    main()
