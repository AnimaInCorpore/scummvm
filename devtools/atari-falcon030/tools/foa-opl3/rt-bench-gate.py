#!/usr/bin/env python3
"""Run the practical OPL kernel on the emulated Falcon DSP, check its frames
against the host reference word for word, and measure its cost.

Four scenarios: the captured Atlantis register stream (the real workload,
with its bursts and its five-of-nine average occupancy), a stress case
holding all nine channels in feedback FM with tremolo and vibrato, a paths
case that adds what music rarely does (a sustain level lowered under a
running decay, increments past half the chip's phase range, a chip reset
under held notes) for exactness alone: its cost means nothing; and a rhythm
case, which opens on rhythm mode at its most expensive (six feedback FM
channels, the bass drum in feedback FM, all four other drums held) and then
walks every shape the rhythm section takes.

Cost is attributed by code range from Hatari's DSP profile, so the host
port waits between chunks (the bench reads every frame back through XBIOS)
are excluded and the figure is what one output frame costs the DSP: the
render stages, the block-boundary pass, event application, the mix clear
and the emit. The cycle model is Hatari's, not hardware: it charges Falcon
external memory zero wait states and calls itself instruction-wise correct
rather than cycle accurate.
"""
import argparse
from datetime import date
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

OSCILLATOR = 32084988
CLOCKS_PER_CYCLE = 2
CODEC_RATE = 25175000.0 / 512.0   # 49,169.92 Hz
BUDGET = OSCILLATOR / CLOCKS_PER_CYCLE / CODEC_RATE

LABEL_RE = re.compile(r"^\s*\d+\s+([A-Za-z_][A-Za-z0-9_]*):\s*(;.*)?$")
ADDRESS_RE = re.compile(r"^\s*\d+\s+P:([0-9A-F]+)\b")
PROFILE_RE = re.compile(r"^p:([0-9a-f]+).*?\s[0-9]+[.,][0-9]+% \((\d+), (\d+), (\d+)\)$")

# Rarely taken paths the paths case must reach, as block counts from the fixture.
PATHS = ("blocks_decaying_past_sustain_level", "blocks_entering_sustain_above_level",
         "blocks_with_negative_increment", "blocks_with_vibrato", "blocks_paused",
         "blocks_holding_attack_zero", "blocks_holding_attack_max")

# The shapes of the rhythm section the rhythm case must reach.
RHYTHM_PATHS = ("blocks_with_drums", "blocks_with_silent_drums", "blocks_with_tom",
                "blocks_with_bass_carrier_alone", "blocks_with_bass_feedback_fm", "blocks_with_bass_plain_fm",
                "blocks_with_drum_vibrato", "blocks_melodic_after_rhythm")

# Code ranges that are the kernel's own per-block work, by listing symbol.
RANGES = {
    "stages": ("stage_mod_plain", "op_boundary"),
    "op_boundary": ("op_boundary", "emit_block_stream"),
    "stream_emit": ("emit_block_stream", "hot_code_end"),
    "trigger_attack": ("trigger_attack", "render_channels"),
    "render_driver": ("render_channels", "load_mod"),
    "loaders_and_modes": ("load_mod", "clear_mix"),
    "clear_mix": ("clear_mix", "emit_block"),
    "emit": ("emit_block", "block_boundary"),
    "block_and_channel_boundary": ("block_boundary", "render_block"),
    "render_block": ("render_block", "apply_events"),
    "apply_events": ("apply_events", "external_code_end"),
}


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


def words(path):
    data = path.read_bytes()
    return [struct.unpack(">I", data[i:i + 4])[0] for i in range(0, len(data), 4)]


def run_case(name, fixture_args, output, vbls):
    case = output / name
    case.mkdir(parents=True)
    fixture = subprocess.run([str(HERE / "build/headless/opl-rt-fixture"), fixture_args[0],
                              str(case / "OPLDATA.BIN"), str(case / "EXPECT.BIN")] + fixture_args[1:],
                             capture_output=True, text=True, check=True)
    shape = json.loads(fixture.stdout)
    shutil.copy(HERE / "build/OPLRT.TOS", case)

    symbols = listing_symbols(HERE / "dsp/OPLRT.LST")
    for label in ("profile_start", "profile_end", "hot_code_end", "external_code_end"):
        if label not in symbols:
            raise SystemExit(f"{label} is missing from the DSP listing")
    (case / "start.ini").write_text(
        f"db pc = ${symbols['profile_start']:04x} :once :trace :file {(case / 'begin.ini').resolve()}\n")
    (case / "begin.ini").write_text(
        "dp on\n"
        f"db pc = ${symbols['profile_end']:04x} :once :trace :file {(case / 'end.ini').resolve()}\n")
    (case / "end.ini").write_text(f"dp save {(case / 'profile.txt').resolve()}\ndp off\nquit 0\n")

    with (case / "debug.log").open("w") as log:
        subprocess.run([str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
                        "--conout", "2", "--tos", str(TOS), "--patch-tos", "true",
                        "--fast-boot", "true", "--fast-forward", "true", "--sound", "off",
                        "--confirm-quit", "false", "--run-vbls", str(vbls),
                        "--parse", str((case / "start.ini").resolve()),
                        "--log-file", str((case / "hatari.log").resolve()), "OPLRT.TOS"],
                       cwd=case, stdout=log, stderr=subprocess.STDOUT, check=True)

    produced, expected = case / "FRAMES.BIN", case / "EXPECT.BIN"
    if not produced.is_file():
        raise SystemExit(f"{name}: the run produced no frames; see {case}/debug.log")
    got, want = words(produced), words(expected)
    if len(got) != len(want):
        raise SystemExit(f"{name}: expected {len(want)} words, got {len(got)}")
    mismatches = [i for i, (a, b) in enumerate(zip(got, want)) if a != b]

    profile = case / "profile.txt"
    if not profile.is_file():
        raise SystemExit(f"{name}: Hatari captured no profile")
    by_address = {}
    for line in profile.read_text(errors="replace").splitlines():
        match = PROFILE_RE.match(line.strip())
        if match:
            by_address[int(match.group(1), 16)] = (int(match.group(2)), int(match.group(3)))
    frames = shape["frames"]
    breakdown = {}
    for key, (first, last) in RANGES.items():
        lo, hi = symbols[first], symbols[last]
        cycles = sum(c for pc, (_, c) in by_address.items() if lo <= pc < hi)
        breakdown[key] = round(cycles / CLOCKS_PER_CYCLE / frames, 2)
    per_frame = round(sum(breakdown.values()), 2)
    return {
        "scenario": name,
        "seconds": shape["seconds"],
        "frames": frames,
        "blocks": shape["blocks"],
        "chunks": shape["chunks"],
        "register_writes": shape["register_writes"],
        "parameter_events": shape["parameter_events"],
        "peak_events_per_chunk": shape["peak_events_per_chunk"],
        "paths_exercised": {key: shape[key] for key in PATHS + RHYTHM_PATHS},
        "frame_words_compared": len(got),
        "mismatches_against_host_kernel": len(mismatches),
        "first_mismatch_frame": mismatches[0] if mismatches else None,
        "bit_exact": not mismatches,
        "instruction_cycles_per_frame": per_frame,
        "instruction_cycles_per_frame_by_range": breakdown,
        "budget_cycles_per_frame": round(BUDGET, 2),
        "budget_use_percent": round(100.0 * per_frame / BUDGET, 1),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True, help="captured opl-writes.ev to replay")
    parser.add_argument("--seconds", type=float, default=4.0)
    parser.add_argument("--rhythm-trace", type=Path,
                        help="a second captured stream, of a game that uses rhythm mode (Cruise for a Corpse)")
    parser.add_argument("--rhythm-from", type=float, default=153.0,
                        help="where its window starts, on the register image the earlier writes left")
    parser.add_argument("--stress-seconds", type=float, default=1.0)
    parser.add_argument("--vbls", type=int, default=60000)
    args = parser.parse_args()
    if not HATARI.is_file():
        parser.error(f"the DSP-calibrated Hatari is not at {HATARI}")
    args.output.mkdir(parents=True, exist_ok=False)

    cases = [
        run_case("stress", ["stress", "--seconds", str(args.stress_seconds)], args.output, args.vbls),
        run_case("paths", ["paths", "--seconds", str(args.stress_seconds)], args.output, args.vbls),
        run_case("rhythm", ["rhythm", "--seconds", str(args.stress_seconds)], args.output, args.vbls),
        run_case("atlantis", ["trace", "--trace", str(args.trace.resolve()), "--seconds", str(args.seconds)],
                 args.output, args.vbls),
    ]
    if args.rhythm_trace:
        cases.append(run_case("cruise", ["trace", "--trace", str(args.rhythm_trace.resolve()),
                                         "--from", str(args.rhythm_from), "--seconds", str(args.seconds)],
                              args.output, args.vbls))
        if not cases[-1]["paths_exercised"]["blocks_with_drums"]:
            raise SystemExit("the rhythm trace's window plays no drums")
    sources = ("dsp/oplrt.asm", "m68k/oplrt.s", "rt-fixture.cpp", "opl-practical.h", "rt-bench-gate.py",
               "generate-tables.py")
    repository = subprocess.run(["git", "-C", str(HERE), "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "-C", str(HERE), "status", "--porcelain"],
                                capture_output=True, text=True, check=True).stdout.strip())
    program_words = None
    for line in (HERE / "dsp/oplrt_image.i").read_text().splitlines():
        if line.startswith("DSP_STAGE2_PROGRAM_WORDS"):
            program_words = int(line.split()[-1])
    result = {
        "date": date.today().isoformat(),
        "gate": "practical OPL kernel on the emulated Falcon DSP56001: exactness and cycle cost",
        "scummvm_commit": repository,
        "scummvm_worktree_dirty": dirty,
        "source_sha256": {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest() for name in sources},
        "trace_sha256": hashlib.sha256(args.trace.read_bytes()).hexdigest(),
        "rhythm_trace_sha256": (hashlib.sha256(args.rhythm_trace.read_bytes()).hexdigest()
                                if args.rhythm_trace else None),
        "rhythm_trace_from_s": args.rhythm_from if args.rhythm_trace else None,
        "dsp_program_words": program_words,
        "reference": "opl-practical.h, scored against the exact kernel by practical-gate.py",
        "cases": cases,
        "budget_basis": {
            "oscillator_hz": OSCILLATOR,
            "clocks_per_instruction_cycle": CLOCKS_PER_CYCLE,
            "codec_rate_hz": CODEC_RATE,
            "note": "Cycles are attributed by code range from Hatari's DSP profile; the host-port"
                    " waits of the bench transport are excluded, SSI output and a production"
                    " transport are not yet included",
        },
        "implemented": ["block-rate envelope with the chip's rates retimed to the codec rate",
                        "tremolo and vibrato at block rate, the vibrato from the decoder's exact"
                        " per-position increments", "negative (aliased) phase increments",
                        "a full reset through parameter events", "feedback, FM and additive connections",
                        "all four OPL2 waveforms and the OPL2's waveform select enable",
                        "rhythm mode: the bass drum and the tom-tom at twice the level, the hi-hat, the"
                        " snare and the cymbal from the two oscillators' phase bits and the noise",
                        "channel skipping when silent",
                        "parameter events applied at block boundaries"],
        "not_implemented": ["SSI output and the period-paced host transport", "PCM mixing",
                            "four-operator mode"],
        "not_established": [
            "No hardware run: every cycle figure is Hatari's model",
            "No audio was auditioned from the DSP; the practical kernel's quality is the host gate's",
        ],
    }
    (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    if any(not case["bit_exact"] for case in cases):
        raise SystemExit("the DSP output differs from the host reference")
    missed = [key for key in PATHS if not cases[1]["paths_exercised"][key]]
    if missed:
        raise SystemExit(f"the paths case no longer reaches: {', '.join(missed)}")
    missed = [key for key in RHYTHM_PATHS if not cases[2]["paths_exercised"][key]]
    if missed:
        raise SystemExit(f"the rhythm case no longer reaches: {', '.join(missed)}")


if __name__ == "__main__":
    main()
