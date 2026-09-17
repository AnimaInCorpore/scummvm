#!/usr/bin/env python3
"""Resample a Munt reference and verify preloaded Falcon DMA PCM in Hatari.

Requires NumPy, VASM/VLINK and the calibrated sibling Hatari checkout.
The comparison is against the resampled PCM, not against Munt's 48 kHz words.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import wave

import numpy as np

HERE = Path(__file__).resolve().parent
RATE = 25175000 / 512


def run(command, cwd, log, env=None):
    with log.open("w") as output:
        subprocess.run(list(map(str, command)), cwd=cwd, env=env, stdout=output,
                       stderr=subprocess.STDOUT, timeout=60, check=True)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_wav(path, samples, rate):
    with wave.open(str(path), "wb") as output:
        output.setparams((2, 2, round(rate), 0, "NONE", "not compressed"))
        output.writeframes(samples.astype("<i2").tobytes())


def resample(samples, source_rate, first, count):
    # 96-tap Kaiser-windowed sinc, evaluated at the exact codec-clock ratio.
    # Source data outside the file is zero; no periodic FFT wrap at the ends.
    if source_rate > RATE:
        raise ValueError("This upsampling-only converter needs source rate <= codec rate")
    output = np.empty((count, 2), dtype=np.int16)
    radius, beta = 48, 9.0
    offsets = np.arange(-radius + 1, radius + 1)
    normalizer = np.i0(beta)
    for start in range(0, count, 4096):
        end = min(start + 4096, count)
        positions = (np.arange(start, end, dtype=np.float64) + first) * (source_rate * 512 / 25175000)
        indices = np.floor(positions).astype(np.int64)[:, None] + offsets
        delta = positions[:, None] - indices
        window = np.i0(beta * np.sqrt(np.maximum(0, 1 - (delta / radius)**2))) / normalizer
        weights = np.sinc(delta) * window * (np.abs(delta) <= radius)
        weights /= weights.sum(axis=1)[:, None]
        valid = (indices >= 0) & (indices < len(samples))
        data = samples[np.clip(indices, 0, len(samples)-1)].astype(np.float64)
        values = np.einsum("ij,ijk->ik", weights * valid, data)
        rounded = np.rint(values)
        if np.any(rounded < -32768) or np.any(rounded > 32767):
            raise ValueError("Resampling would clip; choose a reference with more headroom")
        output[start:end] = rounded.astype(np.int16)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--start", type=float, default=8)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--prepare-only", action="store_true", help="Write resampled PCM/WAV assets for the in-game stream probe without building a resident DMA test")
    parser.add_argument("--f030mt32", type=Path, default=HERE.parents[4] / "F030MT32")
    parser.add_argument("--hatari", type=Path, default=HERE.parents[4] / "F030Arcade/third_party/hatari/build/src/hatari")
    parser.add_argument("--output", type=Path, default=HERE / "build/pcm-gate")
    args = parser.parse_args()
    maximum = 3600 if args.prepare_only else 15
    if not np.isfinite([args.start, args.seconds]).all() or args.start < 0 or not 0 < args.seconds <= maximum:
        parser.error(f"Use a nonnegative start and 0 < seconds <= {maximum}")
    case = args.output.resolve()
    case.mkdir(parents=True, exist_ok=True)
    root = args.f030mt32.resolve()
    with wave.open(str(args.reference), "rb") as source:
        if source.getnchannels() != 2 or source.getsampwidth() != 2:
            parser.error("Reference must be 16-bit stereo PCM WAV")
        source_rate = source.getframerate()
        samples = np.frombuffer(source.readframes(source.getnframes()), dtype="<i2").reshape(-1, 2)
    if args.start + args.seconds > len(samples) / source_rate:
        parser.error("Requested excerpt exceeds reference")
    first, count = round(args.start * RATE), round(args.seconds * RATE)
    expected = resample(samples, source_rate, first, count)
    if not np.any(expected):
        parser.error("Silent excerpt is not a useful transport gate")
    expected.astype(">i2").tofile(case / "SOURCE.RAW")
    reference_first = round(first * source_rate / RATE)
    reference_count = round(count * source_rate / RATE)
    write_wav(case / "reference-excerpt.wav",
              samples[reference_first:reference_first+reference_count], source_rate)
    write_wav(case / "expected.wav", expected, RATE)
    if args.prepare_only:
        print(json.dumps(dict(source=str(case / "SOURCE.RAW"), frames=count,
                              seconds=count / RATE, source_pcm_sha256=digest(case / "SOURCE.RAW")), indent=2))
        return
    # Extra recording room permits whole-frame startup latency; compare the
    # complete excerpt, including its last sample, without accepting omissions.
    output_bytes = (count + 4096) * 4
    (case / "config.i").write_text(f"OUTPUT_BYTES equ {output_bytes}\n")
    run([root / "build/tools/vasm/vasmm68k_mot", HERE / "pcm-host.s", "-quiet", "-Felf", "-m68030",
         f"-I{root / 'src/m68k'}", f"-I{case}", "-o", case / "host.o"], case, case / "vasm.log")
    run([root / "build/tools/vlink/vlink", case / "host.o", "-b", "ataritos", "-s", "-e", "start",
         "-o", case / "PCM.TOS"], case, case / "vlink.log")
    (case / "start.ini").write_text(f"b d7 = $13579b :once :trace :file {case / 'begin.ini'}\n")
    (case / "begin.ini").write_text(
        f"r\ninfo crossbar\nprofile on\nb d7 = $2468ac :once :trace :file {case / 'end.ini'}\n")
    (case / "end.ini").write_text(f"profile save {case / 'cpu.txt'}\nprofile off\n")
    for filename in ("OUTPUT.RAW", "STATS.RAW", "cpu.txt"):
        (case / filename).unlink(missing_ok=True)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    run([args.hatari.resolve(), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
         "--cpuclock", "16", "--cpu-exact", "true", "--tos", root / "third_party/f030dsp3d/tools/tos402.rom",
         "--patch-tos", "true", "--fast-boot", "true", "--fast-forward", "true", "--sound", "off",
         "--confirm-quit", "false", "--run-vbls", "1400", "--trace", "gemdos", "--trace-file", case / "trace.txt",
         "--parse", case / "start.ini", "PCM.TOS"], case, case / "hatari.log", env)
    trace = (case / "trace.txt").read_text()
    if "Pterm0" not in trace or "Pterm(1)" in trace:
        raise SystemExit(f"Falcon program failed; inspect {case / 'trace.txt'}")
    actual = np.fromfile(case / "OUTPUT.RAW", dtype=">i2").reshape(-1, 2)
    if actual.nbytes != output_bytes:
        raise SystemExit("Capture size mismatch")
    matches = [offset for offset in range(4097)
               if np.array_equal(actual[offset:offset+32], expected[:32])]
    alignment = next((offset for offset in matches
                      if np.array_equal(actual[offset:offset+count], expected)), None)
    if alignment is None:
        raise SystemExit(f"Digital capture does not contain the complete PCM excerpt: {case}")
    ticks = np.fromfile(case / "STATS.RAW", dtype=">u4")
    elapsed = (int(ticks[1]) - int(ticks[0])) / 200
    duration = count / RATE
    if abs(elapsed - duration) > 0.05:
        raise SystemExit(f"DMA duration mismatch: {elapsed} vs {duration}")
    cpu = (case / "cpu.txt").read_text()
    cpu_hz = int(re.search(r"Cycles/second:\s*(\d+)", cpu)[1])
    if cpu_hz != 16042494:
        raise SystemExit(f"Unexpected CPU clock: {cpu_hz}")
    captured = actual[alignment:alignment+count]
    write_wav(case / "captured.wav", captured, RATE)
    report = dict(reference_sha256=digest(args.reference), source_rate=source_rate,
                  codec_rate_numerator=25175000, codec_rate_denominator=512, codec_rate_hz=RATE,
                  preview_wav_rate_hz=round(RATE), excerpt_start_seconds=first/RATE,
                  frames=count, seconds=duration, hz200_elapsed_seconds=elapsed,
                  exact_stereo_words=int(captured.size), startup_frames=alignment,
                  source_pcm_sha256=digest(case / "SOURCE.RAW"), capture_wav_sha256=digest(case / "captured.wav"),
                  pcm_bytes_per_second=RATE*4, source_and_capture_ram_bytes=expected.nbytes+output_bytes,
                  cpu_hz=cpu_hz, ram_mb=14, tos="4.02", dsp_used=False,
                  resampler="96-tap Kaiser-windowed sinc, beta=9, exact codec ratio, rounded PCM16",
                  omissions=["ScummVM integration", "disk streaming", "speech mixing", "interactive transitions",
                             "physical Falcon/DAC measurement", "listening judgement"])
    (case / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
