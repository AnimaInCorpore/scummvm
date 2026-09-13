#!/usr/bin/env python3
"""Assemble and check a real 68030/DSP sample-streaming gate in Hatari.

This deliberately precedes SSI/game integration: a failed render-to-RAM
deadline rejects this transport path without claiming a finished player.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import numpy as np
from bank import RATE, wav

HERE = Path(__file__).resolve().parent


def run(command, cwd, log, env=None):
    with log.open("w") as f:
        subprocess.run(list(map(str, command)), cwd=cwd, env=env, stdout=f,
                       stderr=subprocess.STDOUT, check=True, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--f030mt32", type=Path, default=HERE.parents[4] / "F030MT32")
    parser.add_argument("--hatari", type=Path,
                        default=HERE.parents[4] / "F030Arcade/third_party/hatari/build/src/hatari")
    parser.add_argument("--dosbox", default=shutil.which("dosbox-staging"))
    parser.add_argument("--bank", type=Path, default=HERE / "build/bank")
    parser.add_argument("--voices", type=int, choices=range(1, 33), default=32)
    parser.add_argument("--blocks", type=int, choices=range(1, 257), default=8)
    parser.add_argument("--wrap", action="store_true")
    parser.add_argument("--chunked", action="store_true", help="split at loop ends outside the sample loop")
    parser.add_argument("--packed12", action="store_true", help="normalize and prepack two signed 12-bit samples per host word")
    args = parser.parse_args()
    root = args.f030mt32.resolve()
    sys.path.insert(0, str(root / "tools"))
    from generate_dsp_stage2 import make_boot_image
    from profile_dsp import parse_profile, parse_listing, require_symbol
    case = HERE / f"build/bench-{args.voices}-{'wrap' if args.wrap else 'attack'}{'-chunked' if args.chunked else ''}{'-12bit' if args.packed12 else ''}"
    case.mkdir(parents=True, exist_ok=True)
    for name in ("ASM56000.EXE", "CLDLOD.EXE", "DOS4GW.EXE", "ioequ.inc"):
        shutil.copyfile(root / "third_party/f030dsp3d/tools/asm56k" / name, case / name)
    shutil.copyfile(HERE / "mix.asm", case / "MIX.ASM")
    (case / "config.inc").write_text(f"VOICES equ {args.voices}\nPACKED12 equ {int(args.packed12)}\n")
    (case / "BUILD.BAT").write_text(
        "@ECHO OFF\nASM56000.EXE -q -a -bMIX.CLD -z -lMIX.LST MIX.ASM\n"
        "IF ERRORLEVEL 1 EXIT 1\nCLDLOD.EXE MIX.CLD > MIX.LOD\nEXIT\n")
    for file in ("MIX.CLD", "MIX.LST", "MIX.LOD"):
        (case / file).unlink(missing_ok=True)
    run([args.dosbox, "--noprimaryconf", "--set", "output=texture", case / "BUILD.BAT"],
        HERE, case / "assembler.log")
    listing = (case / "MIX.LST").read_text()
    if not all(re.search(rf"^0 +{s}$", listing, re.M) for s in ("Errors", "Warnings")):
        raise SystemExit(f"Assembler failed: {case / 'MIX.LST'}")
    boot = make_boot_image(case / "MIX.LOD", limit=512, purpose="sample mixer")
    (case / "image.i").write_text(
        f"MIX_BOOT_WORDS equ {len(boot)}\n        data\nmix_boot:\n" + "".join(
            f"        dc.b ${(w >> 16) & 255:02x},${(w >> 8) & 255:02x},${w & 255:02x}\n"
            for w in boot) + "        even\n")
    total = args.blocks * 512
    (case / "config.i").write_text(
        f"VOICES equ {args.voices}\nBLOCKS equ {args.blocks}\nOUTPUT_BYTES equ {total*4}\n" +
        f"INPUT_WORDS equ {256 if args.packed12 else 512}\n" +
        ("CHUNKED equ 1\n" if args.chunked else "") + ("PACKED12 equ 1\n" if args.packed12 else ""))
    manifest = json.loads((args.bank / "bank.json").read_text())
    entries = manifest["entries"]
    expected = np.zeros((total, 2), dtype=np.int64)
    lines = ["voices:"]
    samples = []
    quantization_snr = []
    for voice in range(args.voices):
        entry = entries[voice % len(entries)]
        data = np.fromfile(args.bank / (entry["id"] + ".s16le"), dtype="<i2").astype(np.int64)
        scale = 1.
        if args.packed12:
            data = data[:len(data)//2*2]
            scale = max(float(abs(data).max()) / 32752, 1/32752)
            quantized = np.clip(np.rint(data/scale/16), -2048, 2047).astype(np.int64)*16
            quantization_snr.append(float(10*np.log10(np.mean(data.astype(float)**2) /
                                                     (np.mean((data-quantized*scale)**2)+1e-20))))
            data = quantized
        start = len(data)-128 if args.wrap else 0
        loop = entry["loop_start"] // 2 * 2 if args.packed12 else entry["loop_start"]
        # Different gain and pan per voice. 1/16 total gain reserves headroom.
        left = round((voice % 7 + 1) * (1 << 16) * scale)
        right = round((8 - voice % 7) * (1 << 16) * scale)
        lines += [f"        dc.l sample{voice}+{start*2},sample{voice}+{loop*2},end{voice},${left:06x},${right:06x}"]
        positions = np.arange(total) + start
        positions = np.where(positions < len(data), positions,
                             loop+(positions-len(data)) % (len(data)-loop))
        expected[:, 0] += data[positions]*left // (1 << 23)
        expected[:, 1] += data[positions]*right // (1 << 23)
        samples += [f"sample{voice}:", "        dc.w " + ",".join(str(int(x)) for x in data[:8])]
        # Binary include holds original RAM-size samples, never expanded periods.
        if args.packed12:
            q = data // 16 & 4095
            ((q[::2] << 12) | q[1::2]).astype(">u4").tofile(case / f"S{voice:02d}.RAW")
        else:
            data.astype(">i2").tofile(case / f"S{voice:02d}.RAW")
        samples[-1] = f'        incbin "S{voice:02d}.RAW"'
        samples += [f"end{voice}:"]
    (case / "bank.i").write_text("\n".join(lines + samples) + "\n")
    run([root / "build/tools/vasm/vasmm68k_mot", HERE / "host.s", "-quiet", "-Felf", "-m68030",
         f"-I{root / 'src/m68k'}", f"-I{case}", "-o", case / "host.o"], HERE, case / "vasm.log")
    run([root / "build/tools/vlink/vlink", case / "host.o", "-b", "ataritos", "-s", "-e", "start",
         "-o", case / "MIX.TOS"], HERE, case / "vlink.log")
    (case / "start.ini").write_text(f"b d7 = $13579b :once :trace :file {case / 'begin.ini'}\n")
    (case / "begin.ini").write_text(
        f"r\nprofile on\ndp on\nb d7 = $2468ac :once :trace :file {case / 'end.ini'}\n")
    (case / "end.ini").write_text(
        f"profile save {case / 'cpu.txt'}\nprofile off\ndp save {case / 'dsp.txt'}\ndp off\n")
    for name in ("cpu.txt", "dsp.txt", "OUTPUT.RAW"):
        (case / name).unlink(missing_ok=True)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    run([args.hatari, "--machine", "falcon", "--dsp", "emu", "--memsize", "14", "--cpuclock", "16",
         "--cpu-exact", "true", "--tos", root / "third_party/f030dsp3d/tools/tos402.rom",
         "--patch-tos", "true", "--fast-boot", "true", "--fast-forward", "true", "--sound", "off",
         "--confirm-quit", "false", "--run-vbls", "900", "--trace", "gemdos", "--trace-file", case / "trace.txt",
         "--parse", case / "start.ini", "MIX.TOS"], case, case / "hatari.log", env)
    trace = (case / "trace.txt").read_text()
    if "Pterm0" not in trace or "Pterm(1)" in trace:
        raise SystemExit(f"Host failed: {case / 'trace.txt'}")
    actual = np.fromfile(case / "OUTPUT.RAW", dtype=">i2").astype(np.int64).reshape(-1, 2)
    if expected.min() < -32768 or expected.max() > 32767:
        raise SystemExit("Invalid fixture: output clips")
    if actual.shape != expected.shape or not np.array_equal(actual, expected):
        np.save(case / "expected.npy", expected)
        np.save(case / "actual.npy", actual)
        raise SystemExit(f"Output mismatch: {case}")
    cpu = (case / "cpu.txt").read_text()
    hz = int(re.search(r"Cycles/second:\s*(\d+)", cpu)[1])
    rows = re.findall(r"^(?:0x|\$)?[0-9a-f]+\s.*?\([0-9]+, ([0-9]+),", cpu, re.M)
    cycles = sum(map(int, rows))
    if hz != 16042494 or not cycles:
        raise SystemExit(f"Unexpected CPU calibration/profile: {hz}, {cycles}")
    output_start = int(re.search(r"^([0-9a-f]+).*move.w #\$03ff,d4", cpu, re.M)[1], 16)
    output_end = int(re.search(r"^([0-9a-f]+).*dbf.w d6,", cpu, re.M)[1], 16)
    counted_rows = re.findall(r"^([0-9a-f]+)\s.*?\([0-9]+, ([0-9]+),", cpu, re.M)
    download_cycles = sum(int(c) for pc, c in counted_rows if output_start <= int(pc, 16) < output_end)
    dsp_hz, dsp_cycles, dsp_rows = parse_profile(case / "dsp.txt")
    loop_pc = require_symbol(parse_listing(case / "MIX.LST"), "P", "mix_loop")
    visits = sum(n for pc, n, c, p in dsp_rows if pc == loop_pc)
    # First instruction polls; visits exceed sample count when starved. The
    # following HRX read is executed exactly once per input sample.
    reads = sum(n for pc, n, c, p in dsp_rows if pc == loop_pc+2)
    expected_reads = total*args.voices // (2 if args.packed12 else 1)
    if reads != expected_reads:
        raise SystemExit(f"Incorrect input word count: {reads}, expected {expected_reads}")
    wav(case / "mix.wav", actual)
    report = dict(voices=args.voices, blocks=args.blocks, frames=total,
                  loop_wrap_test=args.wrap, chunked=args.chunked, packed12=args.packed12,
                  quantization_snr_db=quantization_snr, cpu_hz=hz, cpu_cycles=cycles,
                  elapsed_ms=cycles*1000/hz, block_ms=cycles*1000/hz/args.blocks,
                  output_download_ms_per_block=download_cycles*1000/hz/args.blocks,
                  deadline_ms=512*1000/RATE, fraction_of_deadline=cycles/hz/(total/RATE),
                  dsp_instruction_cycles=dsp_cycles/2, dsp_profile_hz=dsp_hz,
                  exact_stereo_words=actual.size, input_sample_reads=reads,
                  host_words_per_block=args.voices*((256 if args.packed12 else 512)+2)+1024,
                  omissions=["SSI/DAC", "ScummVM", "speech", "pitch interpolation",
                             "control envelopes", "reverb", "voice allocation"])
    (case / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    main()
