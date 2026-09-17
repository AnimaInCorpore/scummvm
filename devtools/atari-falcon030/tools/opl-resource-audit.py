#!/usr/bin/env python3
"""Inventory indexed Atlantis sound alternatives; does not simulate iMUSE.

Uses the existing FCM parser for SMF validation. Outputs counts and hashes,
never instrument bytes or audio. These counts are not live polyphony or
register-write bandwidth measurements.
"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / "foa-compiled-music"))
from fcm import chunks, read_resource, require, unpack


def indexed_alternatives(game):
    index = bytes(x ^ 0x69 for x in (game / "ATLANTIS.000").read_bytes())
    data = bytes(x ^ 0x69 for x in (game / "ATLANTIS.001").read_bytes())
    directories = {tag: index[a + 8:b] for tag, a, b in chunks(index)}
    dsou = directories[b"DSOU"]
    count, = unpack("<H", dsou)
    require(len(dsou) == 2 + count * 5, "invalid DSOU")
    rooms = dsou[2:2 + count]
    offsets = unpack("<" + "I" * count, dsou, 2 + count)
    require(data[:4] == b"LECF" and unpack(">I", data, 4)[0] == len(data), "invalid LECF")
    outer = list(chunks(data, 8))
    loffs = [data[a + 8:b] for tag, a, b in outer if tag == b"LOFF"]
    require(len(loffs) == 1, "expected one LOFF")
    loff = loffs[0]
    require(len(loff) == 1 + loff[0] * 5, "invalid LOFF")
    bases = dict(unpack("<BI", loff, 1 + 5 * i) for i in range(loff[0]))
    require(len(bases) == loff[0], "duplicate LOFF room")
    ends = {a + 8: b for tag, a, b in outer if tag == b"LFLF"}
    result = {}
    for sound_id, (room, offset) in enumerate(zip(rooms, offsets)):
        if room == 0 or offset in (0, 0xffffffff):
            continue
        require(room in bases and bases[room] in ends, "invalid sound room")
        pos = bases[room] + offset
        tag, size = unpack(">4sI", data, pos)
        require(tag == b"SOUN" and size >= 16 and pos + size <= ends[bases[room]],
                "invalid indexed SOUN")
        base_tag, payload_size = unpack(">4sI", data, pos + 8)
        require(base_tag == b"SOU " and payload_size == size - 16,
                "unsupported sound container")
        cursor, alternatives = pos + 16, {}
        while cursor < pos + size:
            alt_tag, alt_size = unpack(">4sI", data, cursor)
            require(alt_size <= pos + size - cursor - 8, "invalid alternative size")
            require(alt_tag not in alternatives, "duplicate alternative")
            alternatives[alt_tag] = data[cursor + 8:cursor + 8 + alt_size]
            cursor += 8 + alt_size
        require(cursor == pos + size, "invalid alternative boundary")
        result[sound_id] = (room, alternatives)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    resources = indexed_alternatives(args.game)
    counts = Counter(tag.decode("ascii") for _, alts in resources.values() for tag in alts)
    stats = {}
    for tag in (b"ADL ", b"ROL "):
        cues = [read_resource(sound_id, room, alts[tag])
                for sound_id, (room, alts) in resources.items() if tag in alts]
        events = [e for cue in cues for track in cue.tracks for e in track]
        commands = Counter()
        definitions = Counter()
        instrument_cues = set()
        for cue in cues:
            for track in cue.tracks:
                for event in track:
                    payload = event.payload
                    if event.status != 0xf0 or payload[:1] != b"\x7d":
                        continue
                    require(len(payload) >= 3 and payload[-1] == 0xf7, "invalid iMUSE SysEx")
                    command = payload[1]
                    commands[f"0x{command:02x}"] += 1
                    if tag == b"ADL " and command in (16, 17):
                        # sysex_scumm.cpp: part/hardware prefix, plus global ID for 17.
                        nibbles = payload[4 if command == 16 else 5:-1]
                        require(len(nibbles) in (46, 60) and all(x < 16 for x in nibbles),
                                "invalid AdLib instrument definition")
                        definition = bytes((nibbles[i] << 4) | nibbles[i + 1]
                                           for i in range(0, len(nibbles), 2))
                        definitions[definition] += 1
                        instrument_cues.add(cue.sound_id)
        entry = {
            "resources": len(cues), "tracks": sum(len(cue.tracks) for cue in cues),
            "events": len(events), "imuse_commands": dict(sorted(commands.items())),
            "sound_ids": [cue.sound_id for cue in cues],
        }
        if tag == b"ADL ":
            entry.update(instrument_definition_events=sum(definitions.values()),
                         distinct_instrument_definitions=len(definitions),
                         resources_with_instrument_definitions=len(instrument_cues))
        stats[tag.decode("ascii").strip()] = entry
    adl_ids, rol_ids = (set(stats[tag]["sound_ids"]) for tag in ("ADL", "ROL"))
    result = {
        "scope": "Indexed resources, not live iMUSE execution or OPL register traces",
        "game_files": {name: hashlib.sha256((args.game / name).read_bytes()).hexdigest()
                       for name in ("ATLANTIS.000", "ATLANTIS.001")},
        "indexed_sound_resources": len(resources), "alternative_counts": dict(sorted(counts.items())),
        "midi": stats, "rol_without_adl": sorted(rol_ids - adl_ids),
        "adl_without_rol": sorted(adl_ids - rol_ids),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"alternative_counts": result["alternative_counts"],
                      "ADL": {k: v for k, v in stats["ADL"].items() if k != "sound_ids"},
                      "rol_without_adl": result["rol_without_adl"]}, indent=2))


if __name__ == "__main__":
    main()
