# In-game PCM streaming checkpoint

2026-09-13. One scripted **16 MHz Falcon030 / 68882 / 14 MB** Hatari test now
passes the in-game transport gate. This is an opt-in diagnostic executable,
not an interactive music implementation or a whole-game performance claim.

The normal game mixer owns DMA and mixes both the prerecorded music and
actual CD speech. The probe starts after 300 seconds of guest time in the
opening attic, then clicks the left statue at native coordinates `(40, 80)`.
Indiana moves and speaks. Measurement lasts 60 seconds.

## Result

The passing case is `build/in-game-stream-16384-service/`. Generated WAVs,
screenshots, profiles and game data are ignored. Compact provenance and
measurements are preserved in [in-game-results.json](in-game-results.json).

| Measurement | Result |
| --- | --- |
| Output | PCM16 stereo, obtained 49,170 Hz; codec clock 25,175,000 / 512 Hz |
| PCM file / streaming ring | 11,800,780 bytes / 262,144 bytes |
| Disk read chunks | 32,768 bytes; all available whole chunks replenished per service call |
| DMA halves | 16,384 frames each: 333.2 ms each, 131,072 bytes total |
| Measured interval | 60.195 s |
| Consumed source audio | 60.311 s, including queued audio at quit |
| DMA stops / missing PCM bytes | 0 / 0 |
| Minimum mix-finish margin to next IRQ | 30 ms, measured with a 5 ms clock |
| Maximum mix / update gap | 255 ms / 280 ms |
| Mixing and conversion time | 24.125 s, 40.08% of the interval |
| Minimum ring occupancy | 168,140 bytes, about 855 ms of music |
| Maximum reported read duration | 5 ms; GEMDOS host reads do not model a physical disk |
| Speech-active mix callbacks | 9 |
| dlmalloc maximum footprint | 2,949,120 bytes |
| Program text + data + BSS + basepage | 4,587,098 bytes |
| Smallest sampled largest free ST-RAM block | 4,492,944 bytes |
| Game loop entries | 231, about 3.84 iterations/s in this scene |

The memory figures describe different allocations and cannot simply be
added to obtain total peak RAM. `dlmalloc_max_footprint()` is the heap
high-water; the free-block sample is taken once per second. Screens, DMA,
TOS and the 256 KiB stack are additional memory consumers. The complete
program ran under the emulator's 14 MB limit without allocation failure.
The game loop count is not a measurement of unique displayed frames.

## What changed

- The PCM probe's refill now replenishes multiple disk chunks after a mix.
  The previous one-chunk service supplied only 32 KiB after a 64 KiB mix,
  eventually producing silence even though DMA never stopped.
- Equal-rate audio mixing has a dedicated copy loop. It preserves volume,
  stereo routing and 32-bit accumulation but avoids the generic resampler's
  runtime repeat loop. Six native rate-converter tests pass, covering signed
  samples, cached input, uneven callback sizes, EOF, muting, mono/stereo,
  reversed channels, volume and clamping.
- The Atari graphics backend services audio before screen copies and before
  and after screen presentation. These calls do not run engine timer
  callbacks during rendering. Normal timer/event servicing remains in place.
- The probe records mixer timing, IRQ timing, speech activity, source-buffer
  occupancy, disk-read time and allocation statistics. It is compiled only
  into `PCMTEST.PRG` using `probe.mk`; the normal executable has no probe.

## Audio check and failed controls

`analyze-in-game.py` combines timing/occupancy checks with a comparison of
the host WAV against the source. It aligns the stereo difference (L-R),
which cancels centered mono speech, then checks overlapping half-second
windows against one continuous reference clock. It does not re-align after
each potential dropout. The passing run's minimum correlation is
**0.9999986**, with no low-correlation windows or saturated capture words.
Six initial windows have too little stereo energy for correlation; their
recorded stereo difference is also quiet.

The checked range extends to about 59.5 seconds of source audio; the final
two queued DMA blocks are excluded because the harness quits immediately.
This is a host-resampled waveform continuity check, not PCM-word equality,
a speech-quality assessment, a listening judgment or an analog DAC test.
Synthetic controls accept continuous music plus mono speech, and reject a
20 ms inserted gap, a repeated 200 ms block, and a 5 ms deadline margin.

| Case | DMA stops | Missing bytes | Minimum finish margin | Verdict |
| --- | ---: | ---: | ---: | --- |
| 8,192 frames, earlier probe | 201 | 0 | negative | Fail |
| 16,384, earlier one-chunk refill | 0 | 4,456,448 | -160 ms | Fail |
| 16,384, fixed refill only | 71 | 0 | -150 ms | Fail |
| 16,384, faster copy | 0 | 0 | 5 ms | Waveform passes; margin insufficient |
| 8,192, faster copy + graphics servicing | 0 | 0 | 0 ms | Waveform fails |
| 16,384, faster copy + graphics servicing | 0 | 0 | 30 ms | Pass for this scene |

**Zero DMA-stop events do not establish continuous playback.** The existing
mixer programs the next DMA address before completing the mix, and tracks
buffer phase with a wrap flag. A late mix can therefore corrupt output
without producing a stop event. The smaller-buffer failure needs further
DMA phase/deadline investigation before reducing latency. No release
defaults were changed to enable this full-rate music profile.

## Reproduce

First build the normal Falcon target and create the 60-second Munt reference
as described in [README.md](README.md). Cross-mint tools must be on `PATH`.
From the repository root:

```sh
make -C build-falcon030 -j8 -f Makefile \
  -f ../devtools/atari-falcon030/tools/foa-faithful-music/probe.mk pcm-probe
```

Then from this tool directory, using Python with NumPy:

```sh
python3 pcm-gate.py build/opening-60/reference-48.wav \
  --start 0 --seconds 60 --prepare-only --output build/opening-pcm
python3 in-game-gate.py build/opening-pcm/SOURCE.RAW \
  --samples 16384 --delay-ms 300000 --duration-ms 60000 \
  --speech --click 40 80 --output build/my-stream-test
python3 analyze-in-game.py build/my-stream-test
python3 test-in-game-analysis.py
```

Use a fresh output directory per run. `in-game-gate.py` collects diagnostic
data even when playback fails; **`analyze-in-game.py` supplies the acceptance
exit status**. Default game, Hatari and TOS paths refer to the sibling local
checkouts and can be overridden on the runner command line. Hatari's CPU
profile must report exactly 16,042,494 cycles/s for a 16 MHz run.

## Next port work

1. Reduce the graphics and mixer cost and investigate the smaller-buffer
   phase/deadline failure. The current ~666 ms maximum queue latency and
   ~3.84 game-loop iterations/s leave substantial work before a good release.
2. Implement the bounded iMUSE `21 -> 22 + 29 + 30` transition with explicit
   sequencer-to-sample mapping, including queued latency and synth history.
   Test changed trigger timing and additional loop iterations, not only the
   previously captured opening.
3. Audit reachable fades, jumps, overlaps and loops before assuming a finite
   prerendered transition cache can cover the whole game.
4. Measure real Falcon disk stalls and analog playback, other scenes,
   save/load, pauses and speech/music listening quality.
