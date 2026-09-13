# MT-32 sample bank experiment

This is a measured feasibility experiment, **not a ScummVM MIDI backend**.
It captures complete dry MT-32 instruments offline, finds sustain loops,
compares a sample player against Munt, and benchmarks sample streaming and
stereo mixing in actual 68030/DSP56001 machine code under calibrated Hatari.

**DSP-resident sustains now pass a standalone 32-voice SSI output test in
calibrated Hatari, including a burst of 16 recorded attacks.** See the
[resident experiment](RESIDENT.md) for verified output, overload failures and
sound limitations. This remains a wavetable approximation with scripted
controls, without ScummVM or a complete live MIDI player.

The [first experiment](RESULTS.md) found that prepacking approximately doubles
full-sample streaming throughput, which still misses the 32-voice deadline.
No physical Falcon was tested. Preparation runs on the development computer;
the benchmark executable uses only emulated Falcon CPU, RAM and DSP resources.

## Reproduce

From this directory, with a C++ compiler and Python 3.9+ with NumPy installed:

```sh
make
mkdir -p build/captures
./build/capture /path/to/MT32_CONTROL.ROM /path/to/MT32_PCM.ROM build/captures
python3 bank.py
python3 bench.py
python3 bench.py --chunked
python3 bench.py --voices 16 --chunked
python3 bench.py --chunked --packed12
python3 bench.py --chunked --packed12 --wrap --blocks 64
```

`make MUNT=/path/to/munt` overrides the sibling Munt checkout. The benchmark
uses the sibling F030MT32 assembler tools, XBIOS macros, TOS 4.02 and DSP
listing/loader helpers. `--f030mt32`, `--hatari` and `--dosbox` override their
locations. Default Hatari is the DSP-calibrated F030Arcade build, not Homebrew
Hatari. Its host-port timing remains an approximation requiring hardware
validation. No tool or source in a sibling checkout is modified by the benchmark.
The offline capture build reuses the existing mt32-partials build directory.

`build/` holds ROM-derived samples, WAVs, executables and profiles and is ignored
by Git. Capture requires two complete ROM images recognized by Munt. No ROM
or instrument recording is included in the tracked source.

## Artifacts

- `build/captures/captures.tsv`: 18 bank recordings plus six validation notes.
- `build/bank/bank.json`: loop positions, release curves, memory and errors.
- `build/bank/listen.html`: nine reference/sample comparisons with audio players.
- `build/bench-*/MIX.TOS`: standalone Falcon render-to-RAM executable.
- `build/bench-*/mix.wav`: verified emulator output.
- `build/bench-*/result.json`, `cpu.txt`, `dsp.txt`: timing and raw evidence.
- [results.json](results.json): retained numeric results and provenance.
- [RESIDENT.md](RESIDENT.md) and [resident-results.json](resident-results.json):
  resident-table implementation, reproduction commands and measured limits.
- `build/resident-bank/listen.html`: three sustain comparisons.
- `build/resident-32-64-ssi-a16/MIX.TOS`: verified 32-voice / 16-attack SSI demo.

## What is precomputed

`capture.cpp` records programs 50 (Str Sect 3), 92 (Fr Horn 1), and 32 (Fantasy),
using MIDI keys 48, 60 and 72 at velocities 64 and 100. Every note is held for
eight seconds and followed by four seconds of release. Additional key 67 /
velocity 80 and 350 ms key 60 / velocity 100 notes are held out of bank creation.
Munt uses digital-only 32 kHz output with reverb disabled; stereo is averaged
to mono. Each capture starts in a fresh synth to prevent previous-note state.

`bank.py` applies an offline Fourier lowpass and resamples to the Falcon's
24,584.9609375 Hz rate. It finds similar windows separated by 0.4–1.6 seconds
and bakes a 20 ms crossfade into the loop. The retained attack can be much
longer than 150 ms. A 200 Hz release curve is estimated from the long note's
amplitude decay. This preserves neither arbitrary filter movement nor the
original short-note release. Transposition and velocity scaling are deliberately
simple, and validation reports their errors rather than hiding them.

WAV headers round the rate to 24,585 Hz (1.6 ppm difference); all timing
calculations use the exact codec rate. The listening page compares the **16-bit
bank**, before the independent 12-bit packing experiment, with no independent
loudness normalization or reverb.

For `--packed12`, the bank is normalized per sample, quantized to signed 12-bit,
and paired into one 24-bit DSP word held in one 32-bit CPU slot. Gain compensation
is applied in the pan factors. This halves host transfers without increasing
the CPU sample-bank size: two 16-bit samples and one 32-bit slot both use four
bytes. DSP unpacking reconstructs signed 16-bit samples before gain/pan.
Loop starts and ends are rounded down to even sample indices for this format.

## What the original full-sample benchmark does

The 68030 reads resident ST-RAM samples, maintains independent voice cursors,
wraps the real sustain loops, sends each voice's samples and two pan gains,
and downloads/stores the finished stereo block. The DSP receives, optionally
unpacks, and accumulates all voices into an external-RAM stereo buffer.
`--chunked` computes the distance to a loop boundary once per contiguous span
instead of checking it for every sample. Interrupts remain enabled.

Profiles start after DSP boot and before the first voice and stop after the
last stereo store. They include both processors' waiting, transfer, loop
management, mixing, output download, and intervening TOS interrupts. Boot and
file output are excluded. CPU and DSP profiles cover overlapping time and
**must not be added**. Total CPU cycles, including ROM, determine elapsed time.
The benchmark verifies every stereo word against independent Python integer
arithmetic and checks DSP input-read counts. `--wrap` starts each cursor 128
samples before its end and exercises the actual loops over subsequent blocks.

These are sustained root-pitch voices with constant per-voice gains. There is
no real-time pitch interpolation, envelope/control scheduler, release handling,
voice allocation, reverb, SSI/DAC delivery, speech or game integration. Those
omissions prevent treating a successful block test as a complete player.
Here the test already fails on **average** cost, so a worst-period jitter test
cannot turn it into a passing design.
