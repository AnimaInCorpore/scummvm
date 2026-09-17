#!/usr/bin/env python3
"""Compare compiled tracks to the independent historical SMF event reader.

Its old resource scanner is intentionally NOT used: it truncates ROL payloads
by eight bytes and assigns physical scan ordinals instead of game sound IDs.
This gate verifies representation, not execution of the whole iMUSE engine.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

import fcm
from test_fcm import native_hash


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--bank-dump", type=Path, help="also check all normalized timbres and compiled constants")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location("reference_reader", here.parent / "foa-mt32-demand.py")
    reference = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)
    package = args.package.read_bytes()
    sections = fcm.read_sections(package)
    compiled = fcm.decode_score(sections[b"SCOR"])
    resources = fcm.game_resources(args.game)
    fcm.require([c.sound_id for c in compiled] == [r[0] for r in resources], "resource directory differs")
    reference_events = 0
    for cue, (sound_id, room, source) in zip(compiled, resources):
        start = source.index(b"MThd")
        fmt, tracks, division = struct.unpack_from(">HHH", source, start + 8)
        fcm.require((cue.sound_id, cue.room, cue.prefix, cue.midi_format, len(cue.tracks), cue.division)
                    == (sound_id, room, source[:start], fmt, tracks, division), "cue metadata differs")
        position = start + 14
        for track in cue.tracks:
            length = struct.unpack_from(">I", source, position + 4)[0]
            original = list(reference.parse_track(source[position + 8:position + 8 + length]))
            fcm.require([(e.tick, e.status, e.payload) for e in track] == original, "source event stream differs")
            reference_events += len(original)
            position += 8 + length
        fcm.require(position == len(source), "source track coverage differs")
    native = json.loads(subprocess.check_output([here / "build/scan", args.package], text=True))
    fcm.require(native["instruction_fnv1a"] == native_hash(compiled), "C++ consumer digest differs")
    fcm.require(native["events"] == reference_events, "native event count differs")
    result = fcm.report(compiled, sections, len(package))
    result.update({"source_event_comparison": "exact ticks, statuses, payloads, order, and track boundaries",
                   "source_event_comparison_passed": True, "native_consumer": native,
                   "package_sha256": fcm.sha(package), "limitations": [
                       "No live iMUSE execution equivalence claim", "No waveform synthesis or Falcon polyphony measurement",
                       "ROM profile restricted to MT-32 control 1.07 and MT-32 PCM"]})
    if args.bank_dump:
        result["bank_verification"] = json.loads(subprocess.check_output(
            [here / "build/oracle", "check-bank", args.package, args.bank_dump], text=True))
    with args.report.open("x") as output:
        json.dump(result, output, indent=2, sort_keys=True)
        output.write("\n")
    print(json.dumps({"cues": len(compiled), "events": reference_events, "native": native, "passed": True}, indent=2))


if __name__ == "__main__":
    main()
