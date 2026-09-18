#!/usr/bin/env python3
"""Stream the captured Atlantis register stream through the DSP kernel's
production transport on the emulated Falcon, and check that every period
rendered on time and that the emitted words reproduce the host reference.

The stream host boots the kernel, routes the DSP's SSI to the DAC at
49.170 kHz, and submits one 15-block period per refill through the real
protocol: READY handshake, paced host-port blast of the events and PCM
flag, acknowledgement. The kernel counts every period the transmitter plays
without a fresh render - one it caught mid-render, or a replay while the
host is silent - and sums every emitted word; the fixture computes the same
sum from the host reference.

What this establishes: the transport and SSI path run, the double-buffered
handoff keeps up with the 68030 submitting periods as fast as the DSP takes
them, and the stream-mode render is word-exact. What it does not: no audio
was captured or auditioned from Hatari, no game engine was involved, and
the emulator's DSP timing is a model.

With --starve the host withholds every refill for that many video frames
halfway through, and the gate requires the kernel to count the periods the
transmitter replayed meanwhile. The kernel once judged lateness only when
it rendered, so a stall of a second read as no late period at all: nothing
renders while the host is silent. The late count at the end of the stall
must now match the playback time that passed, within a period either way.
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
VASM = MXDRV / "build/tools/vasm/vasmm68k_mot"
VLINK = MXDRV / "build/tools/vlink/vlink"
# One period: 768 frames at the codec's 49,170 Hz (25.175 MHz / 512).
PERIOD_SECONDS = 768 * 512.0 / 25175000.0
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


def build_starved_host(case, periods, vbls):
    """The stream host with every refill withheld for vbls video frames once
    half the periods are in; it reads the counters at the end of the stall
    and times the stall on TOS's 200 Hz clock."""
    source = (HERE / "m68k/oplplay.s").read_text()
    needle = "        bsr     submit_period\n        subq.l  #1,period_count"
    if source.count(needle) != 1:
        raise SystemExit("oplplay.s no longer has the submit the starvation is injected after")
    source = source.replace(needle, f"""        bsr     submit_period
        cmp.l   #{periods - periods // 2 + 1},period_count
        bne     no_starvation
        Supexec starve_ticks
        move.l  d0,starve_start
        move.w  #{vbls - 1},d3
starve_loop:
        Vsync
        dbra    d3,starve_loop
        Supexec starve_ticks
        sub.l   starve_start,d0
        move.l  d0,result_stall_ticks
        move.l  #CMD_STATUS,d0
        bsr     dsp_exchange
        move.l  d0,result_stall_status
no_starvation:
        subq.l  #1,period_count""")
    source = source.replace("submit_period:\n", "starve_ticks:\n        move.l  $4ba,d0\n        rts\n\nsubmit_period:\n", 1)
    source = source.replace("result_checksum: ds.l 1",
                            "result_checksum: ds.l 1\nresult_stall_ticks: ds.l 1\n"
                            "result_stall_status: ds.l 1\nstarve_start: ds.l 1", 1)
    source = source.replace("Fwrite  file_handle,#8,result_status", "Fwrite  file_handle,#16,result_status", 1)
    (case / "oplplay-starved.s").write_text(source)
    subprocess.run([str(VASM), str(case / "oplplay-starved.s"), "-quiet", "-Felf", "-m68030",
                    "-I", str(MXDRV / "src/m68k"), "-I", str(HERE / "dsp"), "-o", str(case / "oplplay.o")],
                   check=True)
    subprocess.run([str(VLINK), str(case / "oplplay.o"), "-b", "ataritos", "-s", "-e", "start",
                    "-o", str(case / "OPLPLAY.TOS")], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--trace", type=Path, help="captured opl-writes.ev (the trace scenario)")
    parser.add_argument("--scenario", choices=("trace", "stress"), default="trace",
                        help="stress: nine feedback FM channels with tremolo and vibrato held, the kernel's worst case")
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--vbls", type=int, default=20000)
    parser.add_argument("--starve", type=int, default=0, metavar="FRAMES",
                        help="withhold every refill for this many video frames halfway through, and require "
                             "the kernel to count the periods the transmitter replayed meanwhile")
    args = parser.parse_args()
    if not HATARI.is_file():
        parser.error(f"the DSP-calibrated Hatari is not at {HATARI}")
    args.output.mkdir(parents=True, exist_ok=False)
    case = args.output

    fixture = subprocess.run([str(HERE / "build/headless/opl-rt-fixture"), args.scenario,
                              str(case / "OPLDATA.BIN"), str(case / "EXPECT.BIN")]
                             + (["--trace", str(args.trace.resolve())] if args.scenario == "trace" else [])
                             + ["--seconds", str(args.seconds), "--play", str(case / "PLAYDATA.BIN")],
                             capture_output=True, text=True, check=True)
    shape = json.loads(fixture.stdout)
    if args.starve:
        build_starved_host(case, shape["periods"], args.starve)
    else:
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
    stall = None
    if args.starve:
        stall_ticks, stall_status = struct.unpack(">II", result_file.read_bytes()[8:16])
        seconds = stall_ticks / 200.0
        # The period in flight when the host went quiet still plays fresh, so
        # the replays are the stall's length in periods, give or take one.
        expected = seconds / PERIOD_SECONDS - 1
        stall = {
            "withheld_frames": args.starve,
            "seconds": seconds,
            "periods_rendered_at_end": stall_status & 0xfff,
            "late_at_end": stall_status >> 12,
            "late_expected": round(expected, 1),
            "counted": abs((stall_status >> 12) - expected) <= 1.5,
        }
    periods_rendered = status & 0xfff
    late = status >> 12
    result = {
        "date": "2026-09-18",
        "gate": "practical OPL kernel stream mode on the emulated Falcon: transport, timing, exactness",
        "source_sha256": {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                          for name in ("dsp/oplrt.asm", "m68k/oplplay.s", "rt-fixture.cpp", "opl-practical.h")},
        "scenario": args.scenario,
        "trace_sha256": hashlib.sha256(args.trace.read_bytes()).hexdigest() if args.trace else None,
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
        "starvation": stall,
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
    if stall is not None:
        if not stall["counted"]:
            raise SystemExit(f"a {stall['seconds']:.3f} s stall counted {stall['late_at_end']} late periods, "
                             f"expected about {stall['late_expected']}")
    elif late:
        raise SystemExit(f"{late} periods were late")


if __name__ == "__main__":
    main()
