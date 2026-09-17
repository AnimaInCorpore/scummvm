import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

import fcm


def vlq(value):
    parts = [value & 127]
    while value >= 128:
        value >>= 7
        parts.insert(0, 128 | (value & 127))
    return bytes(parts)


def resource(tracks, fmt=2, division=480):
    return (b"MDhd\0\0\0\x08\0\0\x80\x7f\0\0\0\x80"
            + b"MThd\0\0\0\x06" + struct.pack(">HHH", fmt, len(tracks), division)
            + b"".join(b"MTrk" + struct.pack(">I", len(t)) + t for t in tracks))


def native_hash(cues):
    h = 2166136261
    def add(data):
        nonlocal h
        for byte in data:
            h = ((h ^ byte) * 16777619) & 0xffffffff
    for c in cues:
        add(struct.pack(">HHHHHH", c.sound_id, c.room, c.midi_format, c.division, len(c.tracks), len(c.prefix)))
        add(c.prefix)
        for track in c.tracks:
            add(struct.pack(">I", len(track)))
            for event in track:
                opcode, payload = fcm.instruction(event)
                add(struct.pack(">IHH", event.tick, opcode, len(payload)))
                add(payload)
    return f"{h:08x}"


class CompilerTests(unittest.TestCase):
    def setUp(self):
        self.eot = b"\0\xff\x2f\0"
        self.cue = fcm.read_resource(21, 4, resource([
            b"\0\x90\x3c\x64\0\xff\x51\x03\x07\xa1\x20\x83\x60\x3c\0" + self.eot,
            b"\0\x91\x43\x50\x78\x81\x43\0" + self.eot]))

    def test_format_two_preserves_track_selection_and_order(self):
        out = fcm.decode_score(fcm.encode_score([self.cue]))
        self.assertEqual(out, [self.cue])
        self.assertEqual([e.status for e in out[0].tracks[0][:2]], [0x90, 0xff])
        self.assertEqual(out[0].tracks[0][2].tick, 480)
        self.assertEqual(out[0].tracks[1][1].tick, 120)

    def test_independent_resource_ids(self):
        second = fcm.read_resource(150, 68, resource([self.eot]))
        cues = fcm.decode_score(fcm.encode_score([self.cue, second]))
        self.assertEqual([(c.sound_id, c.room) for c in cues], [(21, 4), (150, 68)])
        # Exercise real DSOU/LOFF resolution and the mixed size conventions.
        # An unindexed physical copy must not acquire a fabricated sound ID.
        body = resource([b"\0\x90\x3c\x40" + self.eot])
        def chunk(tag, payload):
            return tag + struct.pack(">I", len(payload) + 8) + payload
        alternative = b"ROL " + struct.pack(">I", len(body)) + body
        sound = chunk(b"SOUN", b"SOU " + struct.pack(">I", len(alternative)) + alternative)
        room_header = chunk(b"RMHD", bytes(4))
        room_a = chunk(b"LFLF", room_header + sound + sound)
        room_b = chunk(b"LFLF", room_header + sound)
        first_base = 8 + (8 + 1 + 2 * 5) + 8
        loff = chunk(b"LOFF", bytes([2]) + struct.pack("<BI", 4, first_base)
                     + struct.pack("<BI", 68, first_base + len(room_a)))
        data = chunk(b"LECF", loff + room_a + room_b)
        rooms, offsets = bytearray(151), [0xffffffff] * 151
        rooms[21], rooms[150] = 4, 68
        offsets[21] = offsets[150] = len(room_header)
        index = chunk(b"DSOU", struct.pack("<H", 151) + rooms + struct.pack("<151I", *offsets))
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory)
            (game / "ATLANTIS.000").write_bytes(bytes(x ^ 0x69 for x in index))
            (game / "ATLANTIS.001").write_bytes(bytes(x ^ 0x69 for x in data))
            self.assertEqual(fcm.game_resources(game), [(21, 4, body), (150, 68, body)])
            # The old header-inclusive ROL assumption truncates eight bytes.
            broken = data.replace(alternative[:8], b"ROL " + struct.pack(">I", len(body) - 8))
            (game / "ATLANTIS.001").write_bytes(bytes(x ^ 0x69 for x in broken))
            with self.assertRaises(fcm.FormatError): fcm.game_resources(game)

    def test_all_native_imuse_operations_reconstruct(self):
        for command in fcm.NIBBLED | fcm.RAW_IMUSE:
            # Signed transpose/detune survive as bytes; no eager MIDI conversion.
            args = b"\x07\xf4\x80\xff\x00" if command in fcm.NIBBLED else b"\x00\x7f"
            event = fcm.event_from_instruction(42, 0x7d00 | command, args)
            self.assertEqual(fcm.instruction(event), (0x7d00 | command, args))

    def test_custom_instrument_uses_logical_part(self):
        body = bytes([1]) * 246
        event = fcm.event_from_instruction(50, 0x4100, b"\x07" + body)
        self.assertEqual(event.payload[1], 7)
        self.assertEqual(sum(event.payload[4:-1]) & 127, 0)
        self.assertEqual(fcm.instruction(event), (0x4100, b"\x07" + body))
        c = fcm.Cue(8, 1, self.cue.prefix, 2, 480, [[event, fcm.Event(50, 0xff, b"\x2f")]])
        self.assertEqual(fcm.decode_cue(8, 1, fcm.encode_cue(c)), c)
        self.assertEqual(fcm.custom_definitions([c, c]), body)

    def test_unknown_sysex_and_escape_are_not_dropped(self):
        for event in (fcm.Event(0, 0xf7, b"\x01\x02"), fcm.Event(1, 0xf0, b"\x7d\x7f\xf7")):
            op, payload = fcm.instruction(event)
            self.assertEqual(fcm.event_from_instruction(event.tick, op, payload), event)

    def test_reject_bad_midi(self):
        bad_tracks = [b"\0\x3c\x64" + self.eot, b"\x81\x80\x80\x80\0\x90\x3c\x64" + self.eot,
                      b"\0\x90\x3c", self.eot + b"\0", b"\0\xff\x51\x02\0\0" + self.eot,
                      b"\0\x90\x80\x01" + self.eot, b"\0\x90\x3c\x64"]
        for track in bad_tracks:
            with self.subTest(track=track), self.assertRaises(fcm.FormatError):
                fcm.read_resource(1, 1, resource([track]))
        for body in [resource([self.eot], division=0), resource([self.eot], division=0x8001), resource([self.eot, self.eot], fmt=0)]:
            with self.assertRaises(fcm.FormatError): fcm.read_resource(1, 1, body)

    def test_bad_nibbles_fail_instead_of_losing_fields(self):
        with self.assertRaises(fcm.FormatError):
            fcm.instruction(fcm.Event(0, 0xf0, b"\x7d\0\1\x20\0\xf7"))

    def test_truncation_crc_version_and_overlap(self):
        package = fcm.pack_sections({b"SCOR": fcm.encode_score([self.cue]), b"TEST": b"abc"})
        self.assertEqual(fcm.decode_score(fcm.read_sections(package)[b"SCOR"]), [self.cue])
        for data in (package[:-1], package[:4] + b"\0\x02" + package[6:], package[:-1] + b"x"):
            with self.assertRaises(fcm.FormatError): fcm.read_sections(data)
        broken = bytearray(package)
        broken[16:20] = struct.pack(">I", 0)
        with self.assertRaises(fcm.FormatError): fcm.read_sections(broken)

    def test_event_bounds_and_tick_order(self):
        data = bytearray(fcm.encode_cue(self.cue))
        table = 8 + len(self.cue.prefix)
        offset = fcm.unpack(">I", data, table + 4)[0]
        for field, replacement in [(offset + 12 + 8, 0xffffffff), (table + 4, 0xffffffff), (offset, 999)]:
            bad = bytearray(data)
            bad[field:field + 4] = struct.pack(">I", replacement)
            with self.assertRaises(fcm.FormatError): fcm.decode_cue(21, 4, bad)

    def test_native_reader_and_malformed_control(self):
        binary = Path(__file__).parent / "build/scan"
        self.assertTrue(binary.exists(), "run make test to build the native consumer")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.fcm"
            package = fcm.pack_sections({b"SCOR": fcm.encode_score([self.cue])})
            path.write_bytes(package)
            output = subprocess.check_output([binary, path], text=True)
            self.assertEqual(json.loads(output)["instruction_fnv1a"], native_hash([self.cue]))
            # Recompute section CRC: the consumer must reject the bad record
            # bounds, not merely notice checksum corruption.
            score = bytearray(fcm.read_sections(package)[b"SCOR"])
            cue_offset = fcm.unpack(">I", score, 8)[0]
            table = cue_offset + 8 + len(self.cue.prefix)
            score[table + 4:table + 8] = struct.pack(">I", 0xffffffff)
            path.write_bytes(fcm.pack_sections({b"SCOR": score}))
            self.assertNotEqual(subprocess.run([binary, path], capture_output=True).returncode, 0)

    def test_bank_shares_parameters_without_losing_muted_data(self):
        param = bytes([1] * 58)
        timbre = b"TestPatch " + bytes([0, 0, 3, 0]) + param * 4
        self.assertEqual(len(timbre), 246)
        bank = {b"TIMB": timbre * 158, b"CDEF": b""}
        result = fcm.compile_bank(bank)
        self.assertEqual(len(result[b"PARM"]), 58)
        self.assertEqual(len(result[b"INST"]), 158 * 22)
        self.assertEqual(len(result[b"PART"]), 158 * 4 * 16)
        self.assertEqual(fcm.unpack(">HBBBBHHHHH", result[b"PART"], 32)[-3], 0)


if __name__ == "__main__":
    unittest.main()
