#!/usr/bin/env python3
"""Compare the real ScummVM SMF and FCM parsers over every indexed resource."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import fcm

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.binary.is_file() or not args.package.is_file():
        parser.error("build the parser-test executable and FCM package first")
    args.output.mkdir(parents=True, exist_ok=False)
    resources = fcm.game_resources(args.game)
    fixture = b"FCT1" + struct.pack(">I", len(resources))
    fixture += b"".join(struct.pack(">HI", sound, len(data)) + data for sound, _, data in resources)
    path = args.output / "source.fct"
    path.write_bytes(fixture)
    result = json.loads(subprocess.check_output([args.binary.resolve(), path.resolve(), args.package.resolve()], text=True))
    result.update({"package_sha256": fcm.sha(args.package.read_bytes()),
                   "binary_sha256": fcm.sha(args.binary.read_bytes()),
                   "gate": "ScummVM MidiParser differential, linear and scripted jumps/pause/track changes",
                   "full_imuse_execution": False, "audio_rendered": False})
    (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))

if __name__ == "__main__":
    main()
