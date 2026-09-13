# Measured results, 2026-09-12

Follow-up, 2026-09-13: the [resident sustain experiment](RESIDENT.md) now passes
32 voices through emulated Falcon SSI, with limited recorded-attack traffic.
The full-sample streaming measurements below remain valid for that earlier
design; the follow-up changes the sound representation and transfer pattern.

**Precalculation buys approximately twice the throughput in this sample
prototype. It does not establish 32 real-time voices or a 2x improvement in
faithful MT-32 emulation.**

All runs used the DSP-calibrated Hatari from F030Arcade, a 16 MHz 68030 with
caches enabled (`CACR=$3111`), 14 MB and TOS 4.02. The CPU profiler reports
16,042,494 cycles/second and DSP profiler 32,084,988 oscillator cycles/second.
No host wall-clock timing was used. Physical hardware and ScummVM integration
were not tested.

## Runtime

512 frames at 24,584.9609375 Hz allow **20.826 ms** per block.

| Voices and format | Mean block time | Stereo words checked | Deadline |
| --- | ---: | ---: | --- |
| 32, 16-bit, per-sample loop check | 73.923 ms | 8,192 | fail |
| 32, 16-bit, loop checks per span | 61.341 ms | 8,192 | fail |
| 16, 16-bit, loop checks per span | 31.773 ms | 8,192 | fail |
| 32, normalized/prepacked 12-bit, checks per span | 30.689 ms | 8,192 | fail |
| 32, same packed format, loop-wrap stress over 64 blocks | 29.986 ms | 65,536 | fail |

The packed 32-voice run is slightly cheaper than the unpacked 16-voice run.
It sends 9,280 host words per block instead of 17,472 for unpacked 32 voices,
including gains and stereo download. Moving loop checks outside the inner
loop is a separate optimization. Preliminary build layouts measured 59.9 ms
instead of 61.3 ms for unpacked spans; the conclusion is **approximately 2x**,
not a claim of an exact universal speedup.

Every listed output word equals the independent integer reference exactly,
including signed unpacking and panning. The long wrap case consumes exactly
524,288 packed input words for 32,768 frames at 32 voices. This equality is to
the specified sample algorithm, **not to MT-32 synthesis**.

Output download costs approximately 1.81 ms per block. Even subtracting it
entirely from the packed test still leaves about **28.9 ms**, above 20.8 ms.
Thus replacing the download with SSI output alone would not rescue this
host-streaming implementation. A different input path or much less input
traffic is needed. The result is not an impossibility proof for all Falcon
sample players.

Profiles and commands are reproducible from [the README](README.md).
[results.json](results.json) retains machine-readable measurements and hashes
of the tested sources, binaries and raw profiles. Generated evidence remains
under `build/bench-*` locally.

## Memory and sound

The 18-entry bank occupies **2,036,044 bytes (1.942 MiB)** of mono PCM for only
three patches, before release tables and metadata. The test executable repeats
some sample storage for its independent voice fixtures; a production bank
would share those samples. The two-samples-per-longword packed format uses
approximately the same ST-RAM space as the 16-bit bank.

Normalized 12-bit quantization alone gives **70.10–71.92 dB signal/error ratio**
over these stored samples. That excludes gain-factor rounding, loop alignment
rounding, resampling, sustain-loop changes, sparse key/velocity coverage and
release approximation. It is a quantization measurement, not a listening test.

The larger sound errors come from replacing synthesis with sparse recordings:

| Patch | Spectral difference at recorded key 60 / velocity 100 | Held-out key 67 / velocity 80 |
| --- | ---: | ---: |
| Str Sect 3 | 4.20 dB | 7.08 dB |
| Fr Horn 1 | 2.13 dB | 11.91 dB |
| Fantasy | 4.69 dB | 7.04 dB |

These are RMS log-magnitude STFT differences over reference bins above a
60 dB floor, including attack, sustain and release. They quantify difference;
there is no established perceptual pass/fail threshold. Detuning/phase changes
also affect the measure. Short-note envelope differences are 3.74, 0.95 and
1.86 dB respectively. The automatic loops have not been manually auditioned.

Open `build/bank/listen.html` for nine matched-level comparisons: recorded
pitch/velocity, a held-out pitch/velocity, and early release for each patch.
The page uses the 16-bit bank so loop/timbre errors can be heard separately
from the transport quantization experiment.

## Recommendation after this experiment

Keep per-sample normalization, offline packing and boundary-based streaming:
they deliver a measured benefit. Do **not** integrate this all-voices-through-
the-host-port design as a promised 32-voice backend.

For independently playable notes, the next candidate is a DSP-resident bank
of compact sustain waveforms with limited attack streaming. That eliminates
most continuous host traffic, but shortened loops may lose the motion in
strings and Fantasy, and 32-note simultaneous attacks remain a separate peak
load. Pitch interpolation, DMA/SSI scheduling and sound comparisons still
need a complete benchmark. That was the next experiment at this point;
[the follow-up](RESIDENT.md) establishes a bounded standalone result and
documents the remaining quality, peak-load and integration limits.

For the closest sound in this particular game, pre-rendering complete musical
segments or a few synchronized stems is the stronger alternative. It preserves
the original multi-partial interactions and reduces live mixing to a small
number of streams. iMUSE transitions, overlapping release tails and branching
segments would need explicit handling; a single recording of the whole score
would not reproduce that behavior. This can represent music with 32 original
partials, but it is not 32 arbitrary live note voices.
