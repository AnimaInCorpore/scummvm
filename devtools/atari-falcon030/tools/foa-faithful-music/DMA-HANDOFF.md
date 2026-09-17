# Completed-buffer DMA handoff

2026-09-13. Follow-up to the [in-game streaming checkpoint](IN-GAME.md).
The Falcon game-only backend now submits a DMA half **after** mixing and
PCM conversion finish, and uses the hardware playback pointer to identify
which half is safe to fill. Screen conversion services audio every 16 rows,
and the SCUMM loop services audio between script, dialogue, drawing, actor
and sound stages without dispatching timer callbacks at those points.

## Failure mechanism

The previous backend programmed the next DMA address before generating its
samples. If a mix crossed the next frame boundary, DMA could read that half
while the CPU was still writing it. The ISR's update-pulse counter did not
detect this: entering `update()` was enough to count as having serviced audio.

The instrumented 8,192-frame baseline (`build/in-game-phase-baseline/`)
observed the DMA pointer inside the destination at the end of one conversion.
It reported no DMA stops or missing source bytes. Its waveform happened to
pass, but its minimum finish margin was zero. This exposes an unsafe handoff
even when the captured output happens to remain continuous. Earlier runs
with the same buffer size also failed the waveform check.

## Ownership rule

While the CPU mixes and converts one half, DMA loops the other, already
completed half. Only after the new half is complete does `Setbuffer()` make
it eligible for playback. Before reusing the old half, `Buffptr()` must show
that playback has entered the submitted half.

This check also handles an extra interrupt at DMA startup and a pending wrap
notification from a late mix. Counting interrupts and toggling the expected
half would risk overwriting audio that is still queued. If service is late,
playback can repeat completed audio; the waveform/deadline acceptance gate
still rejects such a run. This change does not promise glitch-free overload.

The new handoff, 16-row graphics batches and engine service calls are guarded by
`ATARI_FALCON_GAME_ONLY`. The normal Falcon executable contains these fixes;
the prerecorded music source and DMA-position diagnostics remain confined
to the supplemental probe executable. No replacement iMUSE backend is enabled.

## Acceptance checks

The probe reads the actual DMA playback position immediately before and
after conversion to the destination half. `analyze-in-game.py` rejects any
observed active destination, alongside its existing deadline, occupancy,
speech-activity and continuous waveform checks. Older reports without the
new counters remain readable and report those checks as unavailable.

Seven synthetic analysis tests pass, including rejecting an active DMA
destination even when the waveform is continuous, and rejecting a capture
with swapped channels. The pointer checks sample the conversion boundaries;
they are not a trace of every hardware read.

Completed-buffer submission and screen batches alone did not make smaller
buffers reliable. An 8,192-frame candidate passed once, but the subsequent
build with the end-position probe moved before submission missed one deadline
by 25 ms and repeated audio. The 4,096-frame stress case missed 43 deadlines
in 20 seconds. Both failed captures showed zero active-destination checks.
These controls distinguish protecting buffer ownership from meeting playback
deadlines, and motivated servicing audio between engine stages too.

## Swapped channels at 8,192 frames

The engine-service candidates (`build/in-game-engine-service-8192*/`) met
every timing check (20-25 ms finish margin, no stops, no active destination)
but failed all 231 measured waveform windows. The failure was a **stereo
channel swap with a one-word phase error**, not dropouts: source left matched
captured right at correlation 1.0000 throughout, and centered mono speech
left a residue in L-R (the remaining low windows sit exactly where speech
starts). The analyzer only aligned L-R, so it reported the swap as hundreds
of low windows. It now aligns both channel orders, reports
`channel_order` and `anchor_correlation`, and fails a `swapped` capture.
Re-analysis labels `in-game-stream-8192-service` and both engine-service runs
as swapped; the earlier passing runs remain normal.

### Mechanism

In Hatari, `Crossbar_SendDataToDAC()` picks left or right from the parity of
`dmaPlay.currentFrame`. That counter was only reset with the emulator; a DMA
start reset the buffer offset but not the frame phase. A traced release boot
(`build/release-dmactl-trace/`, a debugger breakpoint on every `$FF8901`
change) shows exactly two playback stops: uSound's external clock test plays
its 8,820-byte 8-bit mono buffer twice, each time to the natural end of the
buffer (PCs inside `USoundExternalClockTest`'s polling loop, not its
`clr.b`). Hatari sends each mono byte to both channels and checks for the
end of the buffer between the two, so such a buffer always stops with
`currentFrame = 1`. ScummVM's 16-bit stereo start then sends its first word
to the right channel for the rest of the run. After that start the trace has
only `$03`/`$83` (TOS `Setbuffer` toggling the record/play select bit) up to
VBL 13,600: playback never stopped again.

Before the change below, the ISR stopped DMA immediately (`$FF8901 = 0`)
whenever `update()` missed a wrap. During loading that happened hundreds of
times, landing between the two words of a stereo frame about half the time,
so the final phase varied: some runs were normal, some swapped.

### Changes

- **Backend (`ATARI_FALCON_GAME_ONLY`).** On starvation the ISR no longer
  stops DMA. It writes the playback frame registers to a zeroed segment
  allocated after the two halves (a quarter of a half, whole frames), which
  takes effect at the next frame boundary; `update()` restarts as before. The
  first version masked Timer A (IMRA) directly from `update()`, which runs in
  user mode: the MFP is supervisor-only and the program died with a bus error
  at `$FFFFFA13`. Masking, `Setbuffer()` and `Buffoper()` now run together in
  one `Supexec()` call. IMRA masking keeps a pending wrap latched.
- **Probe.** `stops_before_start` counts starvation events before the
  measurement window.
- **Hatari** (`F030Arcade/third_party/hatari`, local branch
  `falcon-dma-frame-phase`). `Crossbar_DmaCtrlReg_WriteByte()` resets
  `dmaPlay.currentFrame` on the 0 -> 1 play transition. Whether a physical
  Falcon keeps the frame phase across a stop is **unverified**; this assumes
  replay starts on a left sample. The standalone PCM transport gate is
  unchanged by it: the capture WAV hash (`538280a9...`), `OUTPUT.RAW` and all
  983,398 exact stereo words match the pre-patch run.

### Result

With the silence switch alone, all three 8,192-frame runs
(`build/in-game-silence2-8192-{a,b,c}/`) came out swapped: without the
random mid-frame stops, uSound's parity survived every time. With the Hatari
fix as well, all three pass (`build/in-game-phase-8192-{a,b,c}/`, delays 300,
305 and 310 s, speech plus click):

| Run | Order | Min finish margin | DMA stops | Missing bytes | Active destination | Low windows | Min correlation | Starvation before start |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| a | normal | 10 ms | 0 | 0 | 0 / 0 | 0 | 0.9999986 | 847 |
| b | normal | 15 ms | 0 | 0 | 0 / 0 | 0 | 0.9999986 | 848 |
| c | normal | 25 ms | 0 | 0 | 0 / 0 | 0 | 0.9999986 | 849 |

All three: 16,042,494 CPU cycles/s, 18 speech-active mixes, maximum mix
130 ms, maximum update gap 150-155 ms, mixing about 40% of the interval,
heap peak 2,883,584 bytes. The release executable boots to gameplay without
exceptions (`build/release-silence2-smoke/`).

This is 8,192 frames (about 167 ms per half, about 333 ms maximum queue) in
one scripted scene under an emulator whose DMA phase was just corrected. It
is not a physical Falcon result. The 5 ms clock limits the margin
resolution, and three runs do not bound the worst case.

## Reproduce

Build the normal target first, then from the repository root:

```sh
make -C build-falcon030 -j8 -f Makefile \
  -f ../devtools/atari-falcon030/tools/foa-faithful-music/probe.mk pcm-probe
```

From this tool directory, with Python and NumPy:

```sh
python3 in-game-gate.py build/opening-pcm/SOURCE.RAW \
  --samples 8192 --delay-ms 300000 --duration-ms 60000 \
  --speech --click 40 80 --output build/my-completed-handoff
python3 analyze-in-game.py build/my-completed-handoff
python3 test-in-game-analysis.py
```

The previous checkpoint documents reference generation and tool paths.
Use a new output directory for each run. Hatari must report 16,042,494 CPU
cycles/s, 14 MB and the same full-rate stereo profile. GEMDOS host reads
still do not model physical disk stalls.

## Remaining fidelity work

This improves the transport foundation for the bounded iMUSE
`21 -> 22 + 29 + 30` transition. That transition still needs sample-position
mapping, continuation audio that preserves synth/reverb history, and tests
with changed trigger timing and extra loop iterations. Whole-game cache
coverage, physical Falcon playback, pauses and save/load remain unverified.

SSI DMA is useful for a future live synth: the separate F030MT32 stereo
transport measurements, summarized in the [polyphony assessment](../../docs/mt32-polyphony-options.md),
suggest it could recover about 2.33 ms per 15.62 ms
period on the 68030. Moving PCM gain and panning onto the DSP is a further
unvalidated opportunity. Transfer savings do not reduce the existing
72/92-cycle DSP waveform kernels enough to fit the full MT-32 partial pool.
The current prerecorded stereo stream already goes directly from ST-RAM
through sound DMA to the codec; no DSP synthesis is used in this test.
