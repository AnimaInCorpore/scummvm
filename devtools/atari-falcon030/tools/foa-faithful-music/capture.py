#!/usr/bin/env python3
"""Record the opening through ScummVM's real iMUSE/MT-32 driver, without input."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True, type=Path, help="DOS CD game data directory")
    parser.add_argument("--milliseconds", type=int, default=30000)
    parser.add_argument("--output", type=Path, default=HERE / "build/opening")
    args = parser.parse_args()
    if not 1000 <= args.milliseconds <= 600000:
        parser.error("Capture duration must be 1–600 seconds")
    game = args.game.resolve()
    if any(c in str(game) for c in "\r\n") or not all((game / f"ATLANTIS.{n}").is_file() for n in ("000", "001")):
        parser.error("Game directory must contain ATLANTIS.000 and ATLANTIS.001")
    binary = HERE / "build/headless/scummvm"
    if not binary.is_file():
        parser.error("Run sh build-trace.sh first")
    case = args.output.resolve()
    if (case / "imuse-midi.ev").exists():
        parser.error("Choose a new output directory; an existing trace will not be overwritten")
    case.mkdir(parents=True, exist_ok=True)
    (case / "scummvm.ini").write_text(
        "[scummvm]\nmusic_driver=null\nnative_mt32=true\n"
        f"foa_capture_ms={args.milliseconds}\nconfirm_exit=false\nautosave_period=0\n"
        "subtitles=true\nspeech_mute=true\n\n[foa-reference]\nengineid=scumm\n"
        f"gameid=atlantis\npath={game}\nplatform=pc\nlanguage=en\n")
    command = [str(binary), "-c", "scummvm.ini", "--debugflags=imuse", "foa-reference"]
    with (case / "scummvm.log").open("w") as log:
        subprocess.run(command, cwd=case, stdout=log, stderr=subprocess.STDOUT,
                       timeout=args.milliseconds / 1000 + 60, check=True)
    trace = case / "imuse-midi.ev"
    if not trace.read_text().endswith("# end\n"):
        raise SystemExit("Trace was not closed successfully")
    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()
    manifest = dict(command=command, capture_limit_ms=args.milliseconds,
                    trace_sha256=digest(trace), binary_sha256=digest(binary),
                    game_sha256={name: digest(game / name) for name in ("ATLANTIS.000", "ATLANTIS.001")},
                    timestamp_resolution_us=1000, clock="host elapsed time",
                    scripted_input=None, speech_muted=True)
    (case / "capture.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(trace)


if __name__ == "__main__":
    main()
