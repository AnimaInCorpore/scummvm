#!/usr/bin/env python3
"""Record the OPL register writes the real AdLib driver makes during play.

The isolated headless executable runs the actual game, resource selection,
iMUSE engine and audio/adlib.cpp driver on a virtual clock. Nothing is
synthesized and no emulator is reached; only the register stream is written.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from gate_env import program

HERE = Path(__file__).resolve().parent
BINARY = program(HERE / "build/headless/scummvm-opl-capture")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True, type=Path, help="DOS CD game data directory")
    parser.add_argument("--milliseconds", type=int, default=60000)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--boot-param", type=int, default=None,
                        help="SCUMM boot parameter, to reach a scene other than the opening")
    args = parser.parse_args()
    if not 1000 <= args.milliseconds <= 600000:
        parser.error("Capture duration must be 1-600 seconds")
    game = args.game.resolve()
    if any(c in str(game) for c in "\r\n") or not all((game / f"ATLANTIS.{n}").is_file() for n in ("000", "001")):
        parser.error("Game directory must contain ATLANTIS.000 and ATLANTIS.001")
    if not BINARY.is_file():
        parser.error("Run sh build-capture.sh first")
    case = args.output.resolve()
    case.mkdir(parents=True, exist_ok=False)
    (case / "scummvm.ini").write_text(
        "[scummvm]\nmusic_driver=adlib\nopl_driver=capture\n"
        f"foa_capture_ms={args.milliseconds}\nconfirm_exit=false\nautosave_period=0\n"
        "subtitles=true\nspeech_mute=true\n\n[foa-adlib]\nengineid=scumm\n"
        f"gameid=atlantis\npath={game}\nplatform=pc\nlanguage=en\n")
    command = [str(BINARY), "-c", "scummvm.ini", "--debugflags=imuse"]
    if args.boot_param is not None:
        command += ["--boot-param", str(args.boot_param)]
    command.append("foa-adlib")
    with (case / "scummvm.log").open("x") as log:
        subprocess.run(command, cwd=case, stdout=log, stderr=subprocess.STDOUT,
                       timeout=args.milliseconds / 1000 + 300, check=True)
    trace = case / "opl-writes.ev"
    if not trace.read_text().endswith("# end\n"):
        raise SystemExit("Trace was not closed successfully")
    log_text = (case / "scummvm.log").read_text()
    if "FCM1 test clock: virtual" not in log_text:
        raise SystemExit("Not the virtual-clock capture executable")
    manifest = dict(command=command, capture_limit_ms=args.milliseconds,
                    clock="virtual, advancing at explicit backend waits",
                    trace_sha256=digest(trace), binary_sha256=digest(BINARY),
                    game_sha256={name: digest(game / name) for name in ("ATLANTIS.000", "ATLANTIS.001")},
                    sound_starts=[int(x) for x in re.findall(r"Starting music (\d+)", log_text)],
                    boot_param=args.boot_param, scripted_user_input=None,
                    speech_muted=True, audio_rendered=False, emulator_reached=False)
    (case / "capture.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(trace)


if __name__ == "__main__":
    main()
