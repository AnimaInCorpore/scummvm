#!/usr/bin/env python3
"""Summarize a recorded Falcon run; do not mistake speech enabled for speech tested."""
# Copyright (C) 2026 Sascha Springer
# SPDX-License-Identifier: GPL-3.0-or-later
# AVI extraction adapted from the preserved ScummVM fork's game-gate.py.

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import wave
from array import array
import sys


def extract_audio(avi, wav):
    rate = channels = bits = None
    stream = None
    pcm = bytearray()
    with avi.open("rb") as source:
        def walk(end):
            nonlocal rate, channels, bits, stream
            while source.tell() + 8 <= end:
                tag = source.read(4)
                size = struct.unpack("<I", source.read(4))[0]
                start = source.tell()
                if tag in (b"RIFF", b"LIST"):
                    if not size or start + size > end:
                        size = end - start
                    source.read(4)
                    walk(start + size)
                elif tag == b"strh":
                    stream = source.read(4)
                elif tag == b"strf" and stream == b"auds":
                    _, channels, rate, _, _, bits = struct.unpack("<HHIIHH", source.read(16))
                elif tag == b"01wb":
                    pcm.extend(source.read(size))
                source.seek(start + size)
                if size & 1 and source.read(4) not in (
                    b"RIFF", b"LIST", b"JUNK", b"00dc", b"00db", b"01wb", b"ix00", b"ix01", b"idx1"
                ):
                    source.seek(start + size + 1)
                else:
                    source.seek(start + size)
        walk(avi.stat().st_size)
    if not rate or not pcm or bits != 16:
        raise ValueError("recording has no supported 16-bit PCM audio")
    with wave.open(str(wav), "wb") as output:
        output.setnchannels(channels)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(pcm)
    samples = array("h", pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    peak = max(abs(value) for value in samples)
    return {"rate": rate, "channels": channels, "seconds": len(samples) / channels / rate,
            "peak_dbfs": 20 * math.log10(max(1, peak) / 32768),
            "clipped_samples": sum(value in (-32768, 32767) for value in samples)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--avi", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    text = args.log.read_text(errors="replace")
    pattern = r"Falcon DSP OPL: (\d+) delivered, (\d+) extensions, (\d+) late, (\d+) speech, (\d+) PCM gaps"
    matches = re.findall(pattern, text)
    if not matches:
        raise SystemExit("no transport counter reports")
    counters = dict(zip(("delivered", "extensions", "late", "speech_periods", "pcm_gap_starts"),
                        map(int, matches[-1])))
    audio = extract_audio(args.avi, args.output.with_suffix(".wav"))
    passed = (counters["late"] == 0 and counters["speech_periods"] > 0
              and counters["pcm_gap_starts"] == 0 and audio["peak_dbfs"] > -40
              and "transport failed" not in text)
    report = {"binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "counter_reports": len(matches), "last_counters": counters,
              "audio": audio, "transport_and_speech_gate_passed": passed,
              "limits": "Last reported counters only; no subjective audition or physical hardware test. "
                        "Extensions can slip iMUSE timing even with no DSP lateness."}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
