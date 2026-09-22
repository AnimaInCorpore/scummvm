#!/usr/bin/env python3
"""Render on the emulated Falcon DSP, check the frames, and profile the loop.

Two things are established per configuration: that the DSP kernel's output is
word-for-word identical to the host reference, and how many DSP instruction
cycles one output frame costs.

The cycle figure is Hatari's model, not hardware. That model charges Falcon
external memory zero wait states and two extra cycles when one instruction
reaches two external spaces, and Hatari's own documentation calls its DSP
emulation instruction-wise correct rather than cycle accurate.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

from gate_env import HATARI, TOS402 as TOS, source_sha256

HERE = Path(__file__).resolve().parent

# The Hatari profile counts oscillator clocks; one DSP instruction cycle is two.
OSCILLATOR = 32084988
CLOCKS_PER_CYCLE = 2
CODEC_RATES = {"24.585 kHz": 24584.96, "32.780 kHz": 32779.9479, "49.170 kHz": 49169.921875}

LABEL_RE = re.compile(r"^\s*\d+\s+([A-Za-z_][A-Za-z0-9_]*):\s*$")
ADDRESS_RE = re.compile(r"^\s*\d+\s+P:([0-9A-F]+)\b")
PROFILE_RE = re.compile(r"^p:([0-9a-f]+).*?\s[0-9]+[.,][0-9]+% \((\d+), (\d+), (\d+)\)$")


def listing_symbols(listing):
    """Program addresses for labels, taken from the assembler listing."""
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


def boot_words():
    """Internal program words the Dsp_ExecBoot image occupies, out of 512."""
    for line in (HERE / "dsp/opl_boot.i").read_text().splitlines():
        if line.startswith("OPL_BOOT_WORDS"):
            return int(line.split()[-1])
    raise SystemExit("the DSP boot image does not declare its length")


def words(path):
    data = path.read_bytes()
    return [struct.unpack(">I", data[i:i + 4])[0] for i in range(0, len(data), 4)]


def run_case(channels, frames, output, vbls):
    case = output / f"ch{channels}"
    case.mkdir(parents=True)
    fixture = subprocess.run([str(HERE / "build/headless/opl-dsp-fixture"), str(channels),
                              str(frames), str(case / "OPLDATA.BIN"), str(case / "EXPECT.BIN")],
                             capture_output=True, text=True, check=True)
    shape = json.loads(fixture.stdout)
    shutil.copy(HERE / "build/OPLBENCH.TOS", case)

    symbols = listing_symbols(HERE / "dsp/OPL.LST")
    for name in ("profile_start", "profile_end"):
        if name not in symbols:
            raise SystemExit(f"{name} is missing from the DSP listing")
    (case / "start.ini").write_text(
        f"db pc = ${symbols['profile_start']:04x} :once :trace :file {(case / 'begin.ini').resolve()}\n")
    (case / "begin.ini").write_text(
        "dp on\n"
        f"db pc = ${symbols['profile_end']:04x} :once :trace :file {(case / 'end.ini').resolve()}\n")
    (case / "end.ini").write_text(f"dp save {(case / 'profile.txt').resolve()}\ndp off\n")

    with (case / "debug.log").open("w") as log:
        subprocess.run([str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
                        "--conout", "2", "--tos", str(TOS), "--patch-tos", "true",
                        "--fast-boot", "true", "--fast-forward", "true", "--sound", "off",
                        "--confirm-quit", "false", "--run-vbls", str(vbls),
                        "--parse", str((case / "start.ini").resolve()),
                        "--log-file", str((case / "hatari.log").resolve()), "OPLBENCH.TOS"],
                       cwd=case, stdout=log, stderr=subprocess.STDOUT, check=True)

    produced, expected = case / "FRAMES.BIN", case / "EXPECT.BIN"
    if not produced.is_file():
        raise SystemExit(f"{channels} channels: the run produced no frames; see {case}/debug.log")
    got, want = words(produced), words(expected)
    if len(got) != len(want):
        raise SystemExit(f"{channels} channels: expected {len(want)} words, got {len(got)}")
    mismatches = sum(1 for a, b in zip(got, want) if a != b)

    profile = case / "profile.txt"
    if not profile.is_file():
        raise SystemExit(f"{channels} channels: Hatari captured no profile")
    clocks = instructions = 0
    for line in profile.read_text(errors="replace").splitlines():
        match = PROFILE_RE.match(line.strip())
        if match:
            instructions += int(match.group(2))
            clocks += int(match.group(3))
    cycles_per_frame = clocks / CLOCKS_PER_CYCLE / frames
    operators = channels * 2
    return {
        "channels": channels,
        "operators": operators,
        "frames": frames,
        "waveforms_resident": shape["waveforms"],
        "left_delayed_carriers": shape["delayed_carriers"],
        "frame_words_compared": len(got),
        "mismatches_against_host_kernel": mismatches,
        "bit_exact": mismatches == 0,
        "instructions_per_frame": round(instructions / frames, 2),
        "instruction_cycles_per_frame": round(cycles_per_frame, 2),
        "instruction_cycles_per_operator": round(cycles_per_frame / operators, 2),
        "budget_use_percent": {name: round(100.0 * cycles_per_frame /
                                           (OSCILLATOR / CLOCKS_PER_CYCLE / rate), 1)
                               for name, rate in CODEC_RATES.items()},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=1024)
    parser.add_argument("--vbls", type=int, default=40000)
    parser.add_argument("--channels", type=int, nargs="+", default=[9, 18])
    args = parser.parse_args()
    if not HATARI.is_file():
        parser.error(f"the DSP-calibrated Hatari is not at {HATARI}")
    args.output.mkdir(parents=True, exist_ok=False)

    cases = [run_case(channels, args.frames, args.output, args.vbls) for channels in args.channels]
    sources = ("dsp/opl.asm", "m68k/oplbench.s", "dsp-fixture.cpp", "opl-kernel.h", "bench-gate.py")
    repository = subprocess.run(["git", "-C", str(HERE), "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "-C", str(HERE), "status", "--porcelain"],
                                capture_output=True, text=True, check=True).stdout.strip())
    result = {
        "date": "2026-09-16",
        "gate": "OPL synthesis on the emulated Falcon DSP56001: exactness and cycle cost",
        "scummvm_commit": repository,
        "scummvm_worktree_dirty": dirty,
        "source_sha256": {name: source_sha256(HERE / name)
                          for name in sources},
        "dsp_program_words": boot_words(),
        "dsp_program_word_limit": 512,
        "reference": "opl-kernel.h, itself bit exact against Nuked-OPL3",
        "cases": cases,
        "budget_basis": {
            "oscillator_hz": OSCILLATOR,
            "clocks_per_instruction_cycle": CLOCKS_PER_CYCLE,
            "note": "Budgets are Hatari's calibrated 16 MIPS figure, one kernel"
                    " sample per output frame",
        },
        "implemented": ["phase generation with the chip's f-number and block scaling",
                        "all eight waveforms through resident tables",
                        "operator feedback", "frequency modulation and additive connection",
                        "the chip's one-frame output pipeline delay on both mixes",
                        "sixteen-bit output saturation"],
        "not_implemented": ["the envelope generator: envelope output is held constant,"
                            " so these figures exclude it entirely",
                            "tremolo and vibrato", "four-operator mode", "rhythm mode",
                            "register decoding on the DSP", "SSI output and host transport"],
        "not_established": [
            "No hardware run: every cycle figure is Hatari's model, which charges"
            " Falcon external memory zero wait states and which its own documentation"
            " calls instruction-wise correct rather than cycle accurate",
            "No audio was auditioned and no output-rate conversion is modelled",
            "The kernel is not optimized; the loop uses no parallel X/Y moves",
        ],
    }
    (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    if any(not case["bit_exact"] for case in cases):
        raise SystemExit("the DSP output differs from the host reference")


if __name__ == "__main__":
    main()
