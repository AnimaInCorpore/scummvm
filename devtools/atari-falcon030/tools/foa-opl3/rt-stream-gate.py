#!/usr/bin/env python3
"""Stream the captured Atlantis register stream through the DSP kernel's
production transport on the emulated Falcon, and check that every period
rendered on time and that the emitted words reproduce the host reference.

The stream host boots the kernel, routes the DSP's SSI to the DAC at
32.780 kHz, and submits one 15-block period per refill through the real
protocol: READY handshake, paced host-port blast of the events and PCM
flag, acknowledgement. The kernel counts periods it could not render before
the transmitter reached them, and sums every emitted word; the fixture
computes the same sum from the host reference.

What this establishes: the transport and SSI path run, the double-buffered
handoff keeps up with the 68030 submitting periods as fast as the DSP takes
them, and the stream-mode render is word-exact. What it does not: no audio
was captured or auditioned from Hatari, no game engine was involved, and
the emulator's DSP timing is a model.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

HERE = Path(__file__).resolve().parent
MXDRV = Path.home() / "Work/F030MXDRV"
HATARI = Path.home() / "Work/F030Arcade/third_party/hatari/build/src/hatari"
TOS = MXDRV / "third_party/f030dsp3d/tools/tos402.rom"

LABEL_RE = re.compile(r"^\s*\d+\s+([A-Za-z_][A-Za-z0-9_]*):\s*(;.*)?$")
ADDRESS_RE = re.compile(r"^\s*\d+\s+P:([0-9A-F]+)\b")


def listing_symbols(listing):
    symbols, pending = {}, []
    for line in listing.read_text(errors="replace").splitlines():
        label = LABEL_RE.match(line)
        if label:
            pending.append(label.group(1))
            continue
        address = ADDRESS_RE.match(line)
        if address and pending:
            for name in pending:
                symbols[name] = int(address.group(1), 16)
            pending.clear()
    return symbols


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--vbls", type=int, default=20000)
    args = parser.parse_args()
    if not HATARI.is_file():
        parser.error(f"the DSP-calibrated Hatari is not at {HATARI}")
    args.output.mkdir(parents=True, exist_ok=False)
    case = args.output

    fixture = subprocess.run([str(HERE / "build/headless/opl-rt-fixture"), "trace",
                              str(case / "OPLDATA.BIN"), str(case / "EXPECT.BIN"),
                              "--trace", str(args.trace.resolve()), "--seconds", str(args.seconds),
                              "--play", str(case / "PLAYDATA.BIN")],
                             capture_output=True, text=True, check=True)
    shape = json.loads(fixture.stdout)
    shutil.copy(HERE / "build/OPLPLAY.TOS", case)

    symbols = listing_symbols(HERE / "dsp/OPLRT.LST")
    if "stream_stopped" not in symbols:
        raise SystemExit("stream_stopped is missing from the DSP listing")
    (case / "start.ini").write_text(
        f"db pc = ${symbols['stream_stopped']:04x} :once :trace :file {(case / 'end.ini').resolve()}\n")
    (case / "end.ini").write_text("quit 0\n")
    with (case / "debug.log").open("w") as log:
        subprocess.run([str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
                        "--conout", "2", "--tos", str(TOS), "--patch-tos", "true",
                        "--fast-boot", "true", "--fast-forward", "true", "--sound", "off",
                        "--confirm-quit", "false", "--run-vbls", str(args.vbls),
                        "--parse", str((case / "start.ini").resolve()),
                        "--log-file", str((case / "hatari.log").resolve()), "OPLPLAY.TOS"],
                       cwd=case, stdout=log, stderr=subprocess.STDOUT, check=True)

    result_file = case / "RESULT.BIN"
    if not result_file.is_file():
        raise SystemExit(f"the run produced no result; see {case}/debug.log")
    status, checksum = struct.unpack(">II", result_file.read_bytes()[:8])
    periods_rendered = status & 0xfff
    late = status >> 12
    result = {
        "date": "2026-09-16",
        "gate": "practical OPL kernel stream mode on the emulated Falcon: transport, timing, exactness",
        "source_sha256": {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                          for name in ("dsp/oplrt.asm", "m68k/oplplay.s", "rt-fixture.cpp", "opl-practical.h")},
        "trace_sha256": hashlib.sha256(args.trace.read_bytes()).hexdigest(),
        "seconds": shape["seconds"],
        "periods_submitted": shape["periods"],
        "periods_rendered": periods_rendered,
        "late_periods": late,
        "register_writes": shape["register_writes"],
        "parameter_events": shape["parameter_events"],
        "checksum_expected": shape["period_checksum"],
        "checksum_dsp": checksum,
        "word_exact": checksum == shape["period_checksum"] and periods_rendered == shape["periods"],
        "on_time": late == 0,
        "not_established": [
            "No audio captured or auditioned from the emulator; the checksum proves the rendered words",
            "The 68030 here does nothing but submit periods; a game's load is not modelled",
            "No hardware run",
        ],
    }
    (case / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    if not result["word_exact"]:
        raise SystemExit("the stream-mode output differs from the host reference")
    if late:
        raise SystemExit(f"{late} periods were late")


if __name__ == "__main__":
    main()
