#!/usr/bin/env python3
"""FCM1 experimental compiler: interactive score and ROM-derived synth data.

No audio rendering. Generated packages contain copyrighted game/ROM data and
belong in ignored build directories. This is not a ScummVM music backend.
"""
import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import zlib


class FormatError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise FormatError(message)


def region(data, start, length):
    require(0 <= start <= len(data) and 0 <= length <= len(data) - start,
            "out-of-bounds data")
    return data[start:start + length]


def unpack(fmt, data, offset=0):
    return struct.unpack(fmt, region(data, offset, struct.calcsize(fmt)))


def chunks(data, start=0, end=None):
    end = len(data) if end is None else end
    require(0 <= start <= end <= len(data), "invalid chunk range")
    while start < end:
        tag, size = unpack(">4sI", data, start)
        require(8 <= size <= end - start, "invalid SCUMM chunk size")
        yield tag, start, start + size
        start += size


def game_resources(game):
    """Resolve actual sound IDs through DSOU and LOFF, never scan for signatures."""
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
    room_offsets = dict(unpack("<BI", loff, 1 + 5 * i) for i in range(loff[0]))
    require(len(room_offsets) == loff[0], "duplicate LOFF room")
    room_ends = {a + 8: b for tag, a, b in outer if tag == b"LFLF"}
    resources = []
    for sound_id, (room, offset) in enumerate(zip(rooms, offsets)):
        if offset in (0, 0xffffffff) or room == 0:
            continue
        require(room in room_offsets, "sound room absent from LOFF")
        base = room_offsets[room]
        require(base in room_ends, "LOFF does not point to LFLF payload")
        pos = base + offset
        tag, size = unpack(">4sI", data, pos)
        require(tag == b"SOUN" and size >= 8 and pos + size <= room_ends[base], "invalid SOUN")
        # readSoundResource(): SOU and its alternatives count payload bytes;
        # SOUN and the enclosing room containers include their eight-byte header.
        base_tag, payload_size = unpack(">4sI", data, pos + 8)
        if base_tag != b"SOU ":
            continue
        require(payload_size == size - 16, "invalid SOU payload size")
        cursor, matches = pos + 16, []
        while cursor < pos + size:
            alt_tag, alt_size = unpack(">4sI", data, cursor)
            require(alt_size <= pos + size - cursor - 8, "invalid sound alternative")
            if alt_tag == b"ROL ":
                matches.append(data[cursor + 8:cursor + 8 + alt_size])
            cursor += 8 + alt_size
        require(cursor == pos + size and len(matches) <= 1, "invalid alternatives")
        if matches:
            resources.append((sound_id, room, matches[0]))
    require(resources and len({r[0] for r in resources}) == len(resources), "invalid sound IDs")
    return resources


@dataclass(frozen=True)
class Event:
    tick: int
    status: int
    payload: bytes


@dataclass
class Cue:
    sound_id: int
    room: int
    prefix: bytes
    midi_format: int
    division: int
    tracks: list


def vlq(data, pos):
    value = 0
    for _ in range(4):
        byte = region(data, pos, 1)[0]
        pos += 1
        value = value * 128 + (byte & 127)
        if byte < 128:
            return value, pos
    raise FormatError("MIDI VLQ exceeds four bytes")


def read_track(data):
    pos, tick, running = 0, 0, None
    events = []
    ended = False
    while pos < len(data):
        delta, pos = vlq(data, pos)
        tick += delta
        require(tick <= 0xffffffff, "tick overflow")
        status = region(data, pos, 1)[0]
        if status >= 128:
            pos += 1
            if status < 0xf0:
                running = status
        else:
            require(running is not None, "running status without channel status")
            status = running
        if status == 0xff:
            meta = region(data, pos, 1)
            size, pos = vlq(data, pos + 1)
            payload = meta + region(data, pos, size)
            pos += size
            if meta == b"\x2f":
                require(size == 0 and pos == len(data), "invalid/trailing end of track")
                ended = True
            if meta == b"\x51":
                require(size == 3 and int.from_bytes(payload[1:], "big") != 0, "invalid tempo")
        elif status in (0xf0, 0xf7):
            size, pos = vlq(data, pos)
            payload = region(data, pos, size)
            pos += size
        else:
            require(0x80 <= status < 0xf0, "unsupported MIDI status")
            size = 1 if status >> 4 in (12, 13) else 2
            payload = region(data, pos, size)
            require(all(x < 128 for x in payload), "invalid channel data")
            pos += size
        events.append(Event(tick, status, payload))
    require(ended, "missing end of track")
    return events


def read_resource(sound_id, room, body):
    # In these v5 ROL resources MDhd's length is its payload length (8),
    # unlike the surrounding SCUMM chunks. Preserve the complete prefix.
    require(body[:4] == b"MDhd", "expected MDhd")
    mdlen, = unpack(">I", body, 4)
    require(mdlen == 8, "unsupported MDhd profile")
    start = 8 + mdlen
    require(region(body, start, 8) == b"MThd\0\0\0\6", "unsupported MIDI header")
    fmt, count, division = unpack(">HHH", body, start + 8)
    require(fmt in (0, 1, 2) and count > 0 and (fmt != 0 or count == 1), "invalid MIDI tracks")
    require(0 < division < 0x8000, "SMPTE division unsupported")
    pos, tracks = start + 14, []
    for _ in range(count):
        tag, size = unpack(">4sI", body, pos)
        require(tag == b"MTrk", "expected MTrk")
        tracks.append(read_track(region(body, pos + 8, size)))
        pos += 8 + size
    require(pos == len(body), "trailing MIDI bytes")
    return Cue(sound_id, room, body[:start], fmt, division, tracks)


# All these commands carry one literal prefix byte before the nibble pairs.
# Prefix bytes that ScummVM ignores are still retained for exact reconstruction.
NIBBLED = {0, 48, 49, 50, 51, 52, 53, 80}
RAW_IMUSE = {1, 2, 64, 81, 96}


def instruction(event):
    status, payload = event.status, event.payload
    if status < 0xf0:
        return status, payload
    if status == 0xff:
        return 0xff00 | payload[0], payload[1:]
    if (status == 0xf0 and len(payload) == 255 and payload[0] == 0x41
            and payload[1] < 16 and payload[2:7] == b"\x16\x12\x04\0\0"
            and payload[-1] == 0xf7 and all(x < 128 for x in payload[1:-1])
            and sum(payload[4:-1]) % 128 == 0):
        # Player::sysEx assigns this to the iMUSE part in the device-id nibble;
        # it is NOT an immediate write to an already allocated hardware part.
        return 0x4100, payload[1:2] + payload[7:253]
    if status == 0xf0 and len(payload) >= 3 and payload[0] == 0x7d and payload[-1] == 0xf7:
        cmd = payload[1]
        args = payload[2:-1]
        if cmd in NIBBLED:
            require(len(args) >= 1 and len(args) % 2 == 1 and all(x < 16 for x in args[1:]),
                    "invalid iMUSE nibble encoding")
            return 0x7d00 | cmd, args[:1] + bytes((args[i] << 4) | args[i + 1] for i in range(1, len(args), 2))
        if cmd in RAW_IMUSE:
            return 0x7d00 | cmd, args
    # Preserve other SysEx and escape packets verbatim; never discard them.
    return 0xf000 | status, payload


def event_from_instruction(tick, opcode, payload):
    if 0x80 <= opcode < 0xf0:
        require(len(payload) == (1 if opcode >> 4 in (12, 13) else 2) and all(x < 128 for x in payload), "invalid short event")
        return Event(tick, opcode, payload)
    if opcode >> 8 == 0xff:
        if opcode == 0xff2f:
            require(not payload, "invalid end of track")
        if opcode == 0xff51:
            require(len(payload) == 3 and int.from_bytes(payload, "big") > 0, "invalid tempo")
        return Event(tick, 0xff, bytes([opcode & 255]) + payload)
    if opcode == 0x4100:
        require(len(payload) == 247 and payload[0] < 16 and all(x < 128 for x in payload), "invalid custom timbre")
        data = b"\x41" + payload[:1] + b"\x16\x12\x04\0\0" + payload[1:]
        return Event(tick, 0xf0, data + bytes([-sum(data[4:]) & 127, 0xf7]))
    if opcode >> 8 == 0x7d:
        cmd = opcode & 255
        require(cmd in NIBBLED | RAW_IMUSE, "unknown native iMUSE opcode")
        if cmd in NIBBLED:
            require(payload, "missing iMUSE prefix")
            payload = payload[:1] + bytes(x for b in payload[1:] for x in (b >> 4, b & 15))
        return Event(tick, 0xf0, bytes([0x7d, cmd]) + payload + b"\xf7")
    require(opcode in (0xf0f0, 0xf0f7), "unknown opcode")
    return Event(tick, opcode & 255, payload)


def encode_cue(cue):
    out = bytearray(struct.pack(">HHHH", cue.midi_format, cue.division, len(cue.tracks), len(cue.prefix)))
    out.extend(cue.prefix)
    directory = len(out)
    out.extend(bytes(8 * len(cue.tracks)))
    pending = []
    for i, track in enumerate(cue.tracks):
        out[directory + i * 8:directory + i * 8 + 8] = struct.pack(">II", len(track), len(out))
        for event in track:
            opcode, payload = instruction(event)
            require(len(payload) <= 65535, "event payload exceeds u16")
            arg = int.from_bytes(payload, "big") if opcode < 0xf0 else 0
            pending.append((len(out) + 8, opcode, payload))
            out.extend(struct.pack(">IHHI", event.tick, opcode, len(payload), arg))
    pool = {}
    for field, opcode, payload in pending:
        if opcode < 0xf0:
            continue
        if payload not in pool:
            pool[payload] = len(out)
            out.extend(payload)
        out[field:field + 4] = struct.pack(">I", pool[payload])
    return bytes(out)


def decode_cue(sound_id, room, data):
    fmt, division, count, mdlen = unpack(">HHHH", data)
    require(fmt in (0, 1, 2) and 0 < division < 0x8000 and count > 0 and (fmt != 0 or count == 1), "invalid cue header")
    prefix = region(data, 8, mdlen)
    require(mdlen == 16 and prefix[:8] == b"MDhd\0\0\0\x08", "invalid MDhd")
    table = 8 + mdlen
    region(data, table, count * 8)
    descriptors = [unpack(">II", data, table + i * 8) for i in range(count)]
    event_end = table + count * 8
    for n, offset in descriptors:
        require(n > 0 and offset == event_end, "noncanonical event table")
        region(data, offset, n * 12)
        event_end += n * 12
    tracks = []
    for n, offset in descriptors:
        track, prev = [], 0
        for j in range(n):
            tick, opcode, length, arg = unpack(">IHHI", data, offset + j * 12)
            require(tick >= prev, "unordered ticks")
            prev = tick
            if opcode < 0xf0:
                require(length in (1, 2) and arg < 1 << (8 * length), "invalid inline MIDI")
                payload = arg.to_bytes(length, "big")
            else:
                require(arg >= event_end, "payload overlaps event tables")
                payload = region(data, arg, length)
            track.append(event_from_instruction(tick, opcode, payload))
        require(track[-1].status == 0xff and track[-1].payload == b"\x2f", "missing end of track")
        require(not any(e.status == 0xff and e.payload[:1] == b"\x2f" for e in track[:-1]), "early end of track")
        tracks.append(track)
    return Cue(sound_id, room, prefix, fmt, division, tracks)


def encode_score(cues):
    out = bytearray(struct.pack(">I", len(cues)) + bytes(12 * len(cues)))
    for i, cue in enumerate(cues):
        out.extend(bytes((-len(out)) % 4))
        encoded = encode_cue(cue)
        out[4 + i * 12:16 + i * 12] = struct.pack(">HHII", cue.sound_id, cue.room, len(out), len(encoded))
        out.extend(encoded)
    return bytes(out)


def decode_score(data):
    count, = unpack(">I", data)
    require(count > 0, "empty score")
    region(data, 4, count * 12)
    cues, end, previous = [], 4 + count * 12, -1
    for i in range(count):
        sound_id, room, offset, size = unpack(">HHII", data, 4 + i * 12)
        require(sound_id > previous and offset == ((end + 3) & ~3), "invalid score directory")
        require(not any(region(data, end, offset - end)), "nonzero alignment padding")
        cues.append(decode_cue(sound_id, room, region(data, offset, size)))
        end, previous = offset + size, sound_id
    require(end == len(data), "trailing score data")
    return cues


def pack_sections(sections):
    out = bytearray(struct.pack(">4sHHI", b"FCM1", 1, len(sections), 0) + bytes(16 * len(sections)))
    for i, (tag, payload) in enumerate(sections.items()):
        require(len(tag) == 4, "invalid section name")
        out.extend(bytes((-len(out)) % 4))
        out[12 + 16 * i:28 + 16 * i] = struct.pack(">4sIII", tag, len(out), len(payload), zlib.crc32(payload))
        out.extend(payload)
    out[8:12] = struct.pack(">I", len(out))
    return bytes(out)


def read_sections(data):
    magic, version, count, size = unpack(">4sHHI", data)
    require(magic == b"FCM1" and version == 1 and size == len(data), "invalid package header")
    region(data, 12, count * 16)
    sections, end = {}, 12 + count * 16
    for i in range(count):
        tag, offset, length, crc = unpack(">4sIII", data, 12 + i * 16)
        require(tag not in sections and offset == ((end + 3) & ~3), "invalid section directory")
        require(not any(region(data, end, offset - end)), "nonzero section padding")
        payload = region(data, offset, length)
        require(zlib.crc32(payload) == crc, "section CRC mismatch")
        sections[tag], end = payload, offset + length
    require(end == size, "trailing package bytes")
    return sections


def parse_bank_dump(data):
    require(data[:4] == b"FMB1", "invalid Munt bank dump")
    pos, out = 4, {}
    while pos < len(data):
        tag, length = unpack(">4sI", data, pos)
        require(tag not in out, "duplicate bank section")
        out[tag] = region(data, pos + 8, length)
        pos += 8 + length
    require(b"CDEF" in out and len(out[b"CDEF"]) % 246 == 0, "invalid custom definition bank")
    expected = {b"TIMB": 158 * 246 + len(out[b"CDEF"]), b"CDEF": len(out[b"CDEF"]), b"PTCH": 128 * 8, b"RHYT": 85 * 4,
                b"SYST": 23, b"WAVE": 128 * 4, b"PCML": 524288,
                b"RLUT": 1024, b"TABL": 2615}
    require(set(out) == set(expected), "unknown/incomplete bank dump")
    for tag, size in expected.items():
        require(len(out[tag]) == size, "invalid bank section " + repr(tag))
    return out


PARTIAL_TYPES = (0, 0, 2, 2, 1, 3, 3, 0, 3, 0, 2, 1, 3)
PAIR_MIX = (0, 1, 0, 1, 1, 0, 1, 3, 3, 2, 2, 2, 2)


def compile_bank(bank):
    """Resolve structure dispatch and share parameters; retain every factory patch.

    Key/velocity/controller-dependent parameters remain live. This does not
    claim the descriptors constitute implemented waveform kernels.
    """
    timbres, parameters, kernels, pool = bytearray(), bytearray(), bytearray(), {}
    timbre_count = len(bank[b"TIMB"]) // 246
    for timbre_id in range(timbre_count):
        timbre = bank[b"TIMB"][timbre_id * 246:(timbre_id + 1) * 246]
        common = timbre[:14]
        require(common[10] < 13 and common[11] < 13 and common[12] < 16, "invalid timbre structure")
        timbres.extend(common)
        for slot in range(4):
            param = timbre[14 + 58 * slot:72 + 58 * slot]
            if param not in pool:
                pool[param] = len(pool)
                parameters.extend(param)
            param_id = pool[param]
            timbres.extend(struct.pack(">H", param_id))
            structure = common[10 + slot // 2]
            pcm = bool(PARTIAL_TYPES[structure] & (2 if slot % 2 == 0 else 1))
            kernel = 2 if pcm else param[4] & 1  # square=0, saw=1, PCM=2
            # Descriptor keeps mute, pair mode, and no-sustain independently.
            kernels.extend(struct.pack(">HBBBBHHHHH", timbre_id, slot, kernel, PAIR_MIX[structure],
                                       slot ^ 1, param_id, param[5] if pcm else 0xffff,
                                       (common[12] >> slot) & 1, common[13], 0))
    out = {k: v for k, v in bank.items() if k != b"TIMB"}
    out.update({b"INST": bytes(timbres), b"PARM": bytes(parameters), b"PART": bytes(kernels)})
    if b"TABL" in bank:
        # Partial::startPartial's velocity response and initSynth's resonance
        # constants, resolved for every input velocity rather than one trace.
        pulse = bank[b"TABL"][2506:2607]
        decay = bank[b"TABL"][2607:2615]
        curves, curve_ids, initial = bytearray(), {}, bytearray()
        for param in pool:
            require(param[6] <= 100 and param[7] <= 14 and param[24] <= 30, "invalid normalized partial")
            curve = bytes(max(0, min(255, (v - 64) * (param[7] - 7) + pulse[param[6]])) for v in range(128))
            if curve not in curve_ids:
                curve_ids[curve] = len(curve_ids)
                curves.extend(curve)
            resonance = param[24] + 1
            initial.extend(struct.pack(">HHHH", curve_ids[curve], (32 - resonance) << 10, decay[resonance >> 2] << 2, 0))
        out.update({b"PVEL": bytes(curves), b"PINI": bytes(initial)})
    # Verify the transformation against the normalized Munt timbres, including
    # muted partial data (a future mutable patch can unmute it).
    rebuilt = bytearray()
    for i in range(timbre_count):
        record = timbres[i * 22:(i + 1) * 22]
        rebuilt.extend(record[:14])
        for ref in unpack(">HHHH", record, 14):
            rebuilt.extend(parameters[ref * 58:(ref + 1) * 58])
    require(rebuilt == bank[b"TIMB"], "timbre reconstruction differs")
    return out


def custom_definitions(cues):
    unique = {}
    for cue in cues:
        for track in cue.tracks:
            for event in track:
                opcode, payload = instruction(event)
                if opcode == 0x4100:
                    unique.setdefault(payload[1:], None)
    return b"".join(unique)


def report(cues, sections, size):
    operations, formats = Counter(), Counter()
    for cue in cues:
        formats[str(cue.midi_format)] += 1
        for track in cue.tracks:
            for event in track:
                opcode, _ = instruction(event)
                operations[f"{opcode:04x}"] += 1
    return {"format": "FCM1", "bytes": size, "cues": len(cues),
            "tracks": sum(len(c.tracks) for c in cues),
            "events": sum(operations.values()), "midi_formats": dict(formats),
            "native_imuse_events": sum(n for op, n in operations.items() if op.startswith("7d")),
            "custom_timbre_events": operations["4100"],
            "unique_custom_timbres": len(custom_definitions(cues)) // 246,
            "operations": dict(sorted(operations.items())),
            "largest_compiled_cue_bytes": max(len(encode_cue(c)) for c in cues),
            "sections": {tag.decode(): len(data) for tag, data in sections.items()},
            "rendered_audio_bytes": 0,
            "game_backend_integrated": False, "live_waveform_renderer_implemented": False}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("compile")
    build.add_argument("--game", type=Path, required=True)
    build.add_argument("--bank", type=Path)
    build.add_argument("--output", type=Path, required=True)
    custom = sub.add_parser("extract-custom")
    custom.add_argument("--game", type=Path, required=True)
    custom.add_argument("--output", type=Path, required=True)
    check = sub.add_parser("inspect")
    check.add_argument("file", type=Path)
    args = parser.parse_args()
    if args.command in ("compile", "extract-custom"):
        require(not args.output.exists(), "output already exists; use a new filename")
        resources = game_resources(args.game)
        cues = [read_resource(*r) for r in resources]
        definitions = custom_definitions(cues)
        if args.command == "extract-custom":
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with args.output.open("xb") as output:
                output.write(definitions)
            print(json.dumps({"custom_timbres": len(definitions) // 246, "bytes": len(definitions)}))
            return
        sections = {b"SCOR": encode_score(cues)}
        require(decode_score(sections[b"SCOR"]) == cues, "score reconstruction differs")
        provenance = {"game_files": {name: sha((args.game / name).read_bytes()) for name in ("ATLANTIS.000", "ATLANTIS.001")},
                      "compiler_sha256": sha(Path(__file__).read_bytes()),
                      "synthesis_profile": "mt32-control-1.07-pcm-mt32" if args.bank else None}
        if args.bank:
            dump = args.bank.read_bytes()
            bank = parse_bank_dump(dump)
            require(bank[b"CDEF"] == definitions, "bank does not cover this score's custom timbres")
            sections.update(compile_bank(bank))
            provenance["bank_dump_sha256"] = sha(dump)
        sections[b"PROV"] = json.dumps(provenance, sort_keys=True).encode()
        package = pack_sections(sections)
        require(read_sections(package) == sections, "package round trip failed")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("xb") as output:
            output.write(package)
    else:
        package = args.file.read_bytes()
        sections = read_sections(package)
        cues = decode_score(sections[b"SCOR"])
    result = report(cues, sections, len(package))
    result["sha256"] = sha(package)
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (FormatError, OSError, KeyError, struct.error) as error:
        raise SystemExit(str(error))
