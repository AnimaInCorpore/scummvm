# Faithful Fate of Atlantis music: reference and PCM transport gate

2026-09-13. Target: unaccelerated Falcon030, 16 MHz, 14 MB, internal audio.

**Precompute complete MT-32 audio offline, then play PCM on the Falcon.**
This preserves the reference model's evolving timbres, pitch modulation,
partial allocation and reverb without synthesizing them during play. It
changes the runtime problem from many partials to one stereo stream.
It does not yet solve the game's interactive soundtrack.

The experiment now has a real post-iMUSE reference and a successful
standalone DMA playback test in calibrated Hatari. **The Falcon ScummVM
build does not yet play this music.** No production game/backend source was
changed to enable a replacement soundtrack.

[Listen to the comparison](listen.html) after reproducing the generated
assets. [results.json](results.json) records the measurements and provenance.
All ROMs, game data, traces, generated audio and binaries remain outside
tracked files or inside the ignored `build/` directory.

## What was measured

| Gate | Result |
| --- | --- |
| Actual game output | 60 seconds captured after ScummVM's iMUSE and MT-32 driver; 2,301 MIDI/SysEx events, 810 note-ons, 171 SysEx |
| Sequencer coverage | Cue 150, then 21; its markers execute a queued fade and start overlapping cues 22, 29 and 30 |
| Reference renderer | Full 32-partial Munt pool; peak observed 32; stereo, reverb and old MT-32 analog-output model |
| Render length | 68.023 s including harness shutdown/release and eight seconds of tail |
| Reference saturation | Zero samples equal to either PCM16 saturation limit; peak absolute sample 16,000 |
| Repeatability | Same trace, build, libc seed and 48-frame render cadence reproduce identical WAV bytes |
| Falcon transport | 491,699 stereo frames / 983,398 sample words in a ten-second excerpt, compared in full |
| Playback format | Signed PCM16 stereo, 25,175,000 / 512 = 49,169.921875 frames/s |
| Runtime traffic | 196,679.6875 bytes/s, about 11.8 MB/minute of recorded audio |
| Test RAM | 3,949,976 bytes for source plus digital capture, excluding program/TOS overhead |
| DSP | No DSP program or synthesis used |

The byte comparison is between **resampled input PCM and DMA recording**.
It does not assert equality with the original 48 kHz Munt samples or with
analog output from a physical Falcon. Startup alignment, duration and the
complete first-to-last stereo frame sequence are checked by `pcm-gate.py`.

The ten-second clip is **preloaded**, so this test measures neither disk
throughput nor streaming stalls. Hatari runs at 16,042,494 CPU cycles/s with
14 MB and TOS 4.02. It routes DMA playback to the DAC and to DMA recording.
Emulated time is checked independently of host fast-forward time.
The test spends its wait loop in Vsync; it is not a game CPU-headroom test.

## Why this reference is different from the raw MIDI export

`trace-midi.cpp` replaces only the final null MIDI device in an isolated
headless analysis build. It advertises an MT-32 device, so ScummVM runs its
real resource selection, iMUSE sequencing, part allocation, MT-32 initialization,
program handling and MIDI/SysEx translation. No physical MIDI is sent.
The normal Falcon build does not load this replacement.

`foa-mt32-demand.py --export` and `mt32-partials` remain resource-demand tools.
The exporter forwards a program from iMUSE part setup but does not implement
all the other fields, hooks, jumps, transposition or fades. Munt ignoring
the raw `0x7D` markers does not make their musical effects happen. A fresh
synth for each resource also loses state across cues. Those renders cannot
serve as the faithful soundtrack reference.

The new trace uses host elapsed time with **1 ms timestamp resolution**.
It captures ScummVM 2026.3.1's behavior, not the original DOS executable's
exact timing. Speech is muted and there is no player input. The capture
limit injects a quit event; the trace marks subsequent shutdown messages
as harness-generated. This ending is not an original musical transition.
Fresh captures have host scheduling variation; they need not have identical
timestamps or audio hashes.

## Explicit Munt profile

The renderer requires `ctrl_mt32_1_07` plus `pcm_mt32`. Other ROM models
need a deliberately selected matching profile rather than silently using
the old MT-32's DAC wiring.

- Integer `BIT16S` renderer, 32 partials, native internal 32 kHz clock.
- `DACInputMode_GENERATION1` and `AnalogOutputMode_ACCURATE` (48 kHz output).
- Reverb enabled, programmed by the captured game SysEx; no separate
  reset between musical cues and no override of the game's reserve table.
- Nice amplitude ramps, nice panning and nice partial mixing disabled.
- MIDI queued at native sample timestamps; short-message cable delay
  enabled, SysEx processed at its captured dispatch time. This is an
  explicit software-reference timing policy, not a complete measured
  MPU-401/cable/physical-device model.
- Fixed C-library random seed 1; 48 output frames per render call.

The default Munt DAC mode and amplitude ramps prioritize sound improvements
over strict hardware behavior; this profile explicitly changes both. The
earlier `build/opening/` trial used those defaults and is superseded by
`build/opening-60/` for fidelity comparisons.

Changing render cadence to 257 frames changes the waveform even with the
same MIDI timestamps. Munt's pitch-timer model consumes shared `rand()`
values, and partial render interleaving changes with the call size. This
is consistent with that source behavior; this experiment does not establish
block-size invariance or cross-platform bit identity. Repeatability is
claimed only for the recorded fixed build, libc and cadence.

Offline conversion uses a 96-tap Kaiser-windowed sinc, beta 9, at the exact
Falcon clock ratio. It retains stereo and rejects integer clipping. The
preview WAV header must round the codec rate to 49,170 Hz (about 1.6 ppm
fast); the Falcon itself uses the exact divider. No lossy codec or
single-cycle sustain approximation is used.

## Reproduce

Dependencies: native C++ build tools and the ScummVM build dependencies,
Python 3 plus NumPy, the sibling F030MT32 checkout (Munt, VASM/VLINK,
`xbios.i`, TOS), and calibrated Hatari from sibling F030Arcade. Pass explicit
paths to the Python gate if those checkouts live elsewhere. `MUNT` can be
overridden when running make.

From this directory, with your game data and ROM paths:

```sh
sh build-trace.sh
python3 capture.py --game /path/to/atlantis-cd --milliseconds 60000 --output build/opening-60
make
build/render-trace /path/to/MT32_CONTROL.ROM /path/to/MT32_PCM.ROM \
  build/opening-60/imuse-midi.ev build/opening-60/reference-48.wav 48
python3 pcm-gate.py build/opening-60/reference-48.wav \
  --start 40 --seconds 10 --output build/pcm-gate-faithful
```

`capture.py` refuses to overwrite an existing trace; use a new output
directory for another capture. Open `listen.html` for the default paths.
The PCM gate also creates a standalone `build/pcm-gate-faithful/PCM.TOS`
with the excerpt embedded. That binary has **not** been tested on physical
hardware. Its capture buffers are diagnostic overhead, not a proposed
runtime music memory budget.

For a repeatability check, render the same trace again with block size 48
and compare complete WAV files. `render-trace` rejects incomplete or invalid
traces and returns failure if the synth is still active after the tail.

## The next acceptance test

Build a bounded in-game prototype of the observed **21 → 22 + 29 + 30**
transition. iMUSE must remain the timing authority. Precompute its audio
states and transition continuations, then demonstrate that the Falcon
selects the matching audio at the marker while preserving the sound already
in progress. Compare against the continuous post-iMUSE Munt reference.

One mixed WAV per resource is insufficient: independent part fades,
transposition, loops, overlapping players and arbitrary jumps change the
required output. Independently rendering stems also changes shared reverb
and the original 32-partial stealing behavior. A limited state/transition
cache may be practical, but completeness and storage growth are unproven.
This transport result does not promise a faithful whole-game cache.

The integrated prototype must also demonstrate sustained reads above the
196.7 kB/s payload rate, measured refill deadlines, bounded buffering and
memory high-water, plus speech/actor load. ScummVM's existing audio backend
must own DMA and mix speech with music. A second program claiming the sound
hardware is only a standalone diagnostic. Physical Falcon playback and a
listening comparison are separate outstanding checks.

The earlier [32-voice resident sample renderer](../mt32-sample-bank/RESIDENT.md)
is useful performance research, but its static sustains and attack joins
are audible approximations. It does not satisfy this fidelity-first target.
