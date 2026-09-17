# Live renderer: exact Falcon resampling

2026-09-15. This checkpoint improves the one-partial `FCMLIVE.PRG` diagnostic.
The target remains a faithful-sounding soundtrack and MIDI/iMUSE effects,
synthesized live from compiled parameters and the original logarithmic ROM
words. Full MT-32 synthesis and live iMUSE-to-synth routing remain unfinished.

## Implementation

`live-resampler.h` replaces the diagnostic's per-output-sample Q16 phase and
remainder calculations with a shared, exact coefficient schedule. The input
clock remains 32,000 Hz and the output clock remains 25,175,000 / 512 Hz.
Their ratio reduces to 16,384 / 25,175, so fractional positions and native
sample advances repeat after exactly 25,175 output frames.

Each 16-bit entry contains the original 15-bit interpolation fraction and
one input-advance bit. The 50,350-byte table is generated once before playback
and shared across instances. It contains only rate-conversion coefficients,
with no game, ROM, note or instrument audio. Playback does one sequential
coefficient read per output frame. No allocation, division, floating point
or 64-bit clock calculation occurs in the sample loop.

The resampler retains its phase and two interpolation samples across output
callbacks, native-buffer refills and schedule wrap. An input advance happens
after writing the output, preserving the old renderer's timing. It stops at
the supplied native-buffer boundary and resumes with the next block. The
probe's unequal stereo routing is retained for its speech-cancelling waveform
check; it is not MT-32 panning or reverb.

## Correctness checks

- **1,409,891 resampler frames** match an independent absolute-time reference.
  Seven output chunk sizes exercise eight complete coefficient periods each,
  independently varied native refills, empty calls and full-scale signed input.
  Every callback also checks the exact number of consumed native samples.
- **6,000,000 stereo output frames** are byte-identical to the saved renderer
  before this change, including scripted bends and note releases. Both runs
  generate 3,905,024 native frames with checksum 442,497,966.
- The Munt comparison still passes **300 note cases / 14,400,000 samples**
  with zero mismatches. The separate ROM-wave kernel comparison passes
  **1,152,000 samples**. These are dry, isolated partial checks.
- The resampler and kernel tests also pass AddressSanitizer and
  UndefinedBehaviorSanitizer on the host.
- `FCMLIVE.PRG` and the portable `LIVEDEMO.TOS` cross-build successfully.
  The standalone harness uses `-m68030 -msoft-float`. The complete game build
  still assumes the optional 68882.

## Measured Falcon result

Calibrated Hatari, 16 MHz 68030, 14 MiB, 68882, RGB output, speech enabled,
and a click at (40, 80) two seconds into measurement:

| Build | DMA-half frames | Test length | Resampler cycles/output frame | Mix ms/generated audio second | Late buffers | Worst finish margin | DMA stops | Gate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Previous renderer | 8,192 | 20 s | 82.74 | 819.8 | 24 | -90 ms | 2 | Fail |
| Coefficient schedule | 8,192 | 20 s | 54.76 | 724.3 | 13 | -55 ms | 0 | Fail |
| Coefficient schedule | 16,384 | 60 s | 55.46 | 684.1 | 4 | -75 ms | 0 | Fail |

The matched 8,192-frame case reduces resampler cycles per output frame by
**33.8%** and mix time per generated audio second by **11.7%**. These are
workload measurements, not a complete-synth speedup or a polyphony prediction.
Normalize by generated audio: the old overloaded run produced only 16.33
seconds of audio in 20.125 seconds, versus 18.66 in 20.025 seconds now. Its
lower raw mix-time fraction does not mean it was faster.

Both new runs match the independently generated native-frame checksum and
record zero writes into an active DMA half at the sampled boundaries. Both
still miss deadlines and repeat audio. The larger buffer produces 58.98
seconds of audio in 60.18 seconds, with a maximum mixer call of 385 ms against
a 333.2 ms half-buffer interval. More buffering has not qualified this path.

The final probe reports 4,658,422 bytes of static program storage and peak
heap use of 5,636,096 bytes (8,192-frame halves) or 5,701,632 bytes (16,384).
The coefficient table itself accounts for 50,350 bytes of shared static RAM.
These figures cover the diagnostic, not a production game/synth memory budget.

The next bottlenecks are the 32-bit mixer accumulation and output conversion,
plus peak synthesis/envelope cost. In the 8,192-frame run, stereo mixer
accumulation costs about 49.2 cycles/output frame and the Atari update function
about 38.8; the ROM-wave renderer costs 41.7. Further work must reduce those
costs and bound the worst block, while preserving synthesis and event timing.
Full voice behavior, custom timbres, synthetic waves, reverb and live iMUSE
routing are still separate implementation work.

[live-results.json](live-results.json) retains the checks, binary/package
identities, timings and scope. The raw profiles and captured audio are in
`build/live-game-resampler-v2/` and `build/live-game-resampler-v2-large/`.
These emulator checks are not a physical-Falcon qualification or an audible
fidelity verdict; host-directory reads do not simulate physical disk stalls.

## Reproduce

From this directory, with a verified FCM package and the recognized complete
MT-32 control 1.07 / PCM ROM pair:

```sh
make live-test build/live-demo
build/live-pcm-test reference /path/to/mt32_ctrl_1_07.rom /path/to/mt32_pcm.rom \
  build/atlantis-v3.fcm build/new-fcm.wav build/new-munt.wav
make live-target-check MINT_CXX=/path/to/cross-mint/bin/m68k-atari-mintelf-g++
env PATH=/path/to/cross-mint/bin:$PATH make -C ../../../../build-falcon030 \
  -f Makefile -f ../devtools/atari-falcon030/tools/foa-compiled-music/live.mk \
  fcm-live-probe
python3 live-game-gate.py build/atlantis-v3.fcm \
  --output build/new-live-game --samples 8192 --delay-ms 300000 \
  --duration-ms 20000 --speech --click 40 80
python3 live-game-gate.py build/atlantis-v3.fcm \
  --output build/new-live-game-large --samples 16384 --delay-ms 300000 \
  --duration-ms 60000 --speech --click 40 80
```

Use new output filenames/directories and a Python environment containing NumPy
and SciPy for the waveform gate. Generated packages, ROMs, audio, executable
copies and full profiles stay in ignored `build/`. The Falcon consumes the
FCM package; the host generates reference PCM independently after each run.
