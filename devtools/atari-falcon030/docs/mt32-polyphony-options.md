# Doubling live polyphony and a 32-voice alternative

Assessment: 2026-09-12; resident follow-up: 2026-09-13.
The user subsequently prioritized faithful Fate of Atlantis music. The
[current experiment](../tools/foa-faithful-music/README.md) renders actual
post-iMUSE output offline and verifies preloaded PCM playback at 49.17 kHz.
The 32-voice sample synthesizer below remains an approximation experiment,
not the selected fidelity target.

Target: the 16 MHz 68030 / 32 MHz DSP56001 Falcon
with 14 MB, including ScummVM running Fate of Atlantis. This is an assessment
of existing measurements and a proposed alternative. A subsequent
[three-patch sample-bank experiment](../tools/mt32-sample-bank/RESULTS.md)
now measures the alternative's sound differences, memory and transfer cost;
it is not an implemented ScummVM synthesizer. The subsequent
[DSP-resident prototype](../tools/mt32-sample-bank/RESIDENT.md) passes a
32-sustain-voice SSI test under calibrated Hatari, including 16 simultaneous
151 ms recorded attacks. Continuous 16-stream traffic and 32-attack bursts
fail. That establishes a bounded sample-renderer result, with substantial
sound and integration work still open.

**A sound-preserving 2x improvement is not established by the current code.**
The strongest alternative to investigate is a **32-voice sample synthesizer
using recordings of complete MT-32 patches**, initially at 24,584.96 Hz.
Thirty-two voices alongside this game remain a development target. No
ready-made backend meeting that target alongside this game was verified.

## What is being counted

An MT-32 has nine instrument parts (eight melodic and rhythm) sharing 32
partials. A note can occupy one to four partials. Thirty-two sample voices
would instead mean 32 simultaneous notes when each note selects one mono
sample; layering, crossfades and release voices must count against that pool.
Neither MIDI channels nor iMUSE's software parts specify sounding polyphony.

This ScummVM build disables Munt. The sibling F030MT32 project has measured
components, but live scheduling, control delivery, ring pairs and a combined
DSP image remain unfinished. Its 3-5 exact or 5-8 approximate synth partials,
plus a separate planning allowance of two CPU PCM partials, are estimates.
There is no demonstrated live-channel baseline here to honestly double.

Sources: [build script](../../../backends/platform/atari/build-falcon030.sh),
[F030MT32 budget](/Users/saschaspringer/Work/F030MT32/docs/la32-budget.md),
[live performance assessment](/Users/saschaspringer/Work/F030MT32/docs/live-performance.md).

## Why the remaining small optimizations do not supply 2x

The existing profiles give 489.40 DSP instruction cycles per 32.78 kHz
output frame. Room reverb costs 83 and transport 14.81, leaving 391.59.
Approximate synth kernels cost 44/53 per square/saw partial before panning;
moving controls add up to about 22 per partial per frame.

Sixteen DSP partials would have only **24.47 cycles each**, including their
pan and controls. Thirty-two would have **12.24**. Even the approximate
waveform generation alone exceeds both allowances. This is arithmetic from
component measurements, not a proof that every other synthesis design fails.

| Change | What the evidence supports |
| --- | --- |
| Half-rate room reverb, projected 83 -> about 45 cycles | About 10% more DSP time for partials; changes the reverb and needs a sound comparison |
| Tighter control derivation | Estimated 2-3 cycles saved per moving partial per frame |
| PCM gain/pan on DSP and DMA delivery | Worth investigating for the separate CPU bottleneck; consumes DSP time and needs complete transfer accounting |
| 24.585 kHz synthesis | 33% more DSP cycles per synthesis frame; changes bandwidth and requires retuning |
| 16.390 kHz synthesis | Twice the cycles per synthesis frame; bandwidth below 8.2 kHz, plus changed aliasing and control/reverb timing |

The last option can plausibly double some **settled-kernel** capacity, but
does not establish twice the complete live notes at comparable sound quality.
For example, retaining the existing per-frame costs gives a cost-only ceiling
of 18 approximate square or 15 saw partials at 16.39 kHz, with 83-cycle reverb,
14.81-cycle transport and 2.5 cycles of pan per partial. Keeping moving
controls at the same cadence in milliseconds makes their cost per new frame
roughly double too. Lowering a codec divider alone also slows pitch, envelopes
and delay times; it is not a correct implementation of lower-rate synthesis.

DMA has a concrete qualification problem: the existing stereo probe passes,
but higher slot counts lose samples under calibrated Hatari. The source
assessment explicitly excludes them from integration. The successfully
measured stereo RX+TX costs 16 DSP cycles/frame, so replacing the existing
14.81-cycle transport is primarily a CPU saving, not free DSP time.

The game, display and speech still require CPU time. The standalone component
estimates do not reserve a measured amount for them.

## Audit of the opening-room CPU report

The subsequently supplied 16/32 MHz report is useful evidence of pacing in
this scene. Inspecting the actual saved profiles, rather than only its
symbol summary, confirms these totals:

| Quantity | 16 MHz | 32 MHz |
| --- | ---: | ---: |
| Profile cycles | 534,415,996 | 1,068,831,984 |
| Cycles/second from profile header | 16,042,494 | 32,084,988 |
| Elapsed emulated seconds | 33.31253 | 33.31253 |
| `waitForTimer` calls | 333 | 333 |
| Engine iterations/second | 9.99624 | 9.99624 |
| Audio interrupt's DMA-stop branch executions | **253** | **2** |

Sources: [16 MHz profile](../../../build-falcon030/hatari-test/profile.txt),
[32 MHz profile](../../../build-falcon030/hatari-test/fast/profile.txt),
and [16 MHz profiler log](../../../build-falcon030/hatari-test/hatari-profile.log).
These are local generated artifacts, not tracked fixtures. No new emulation
run was performed for this audit.

The earlier 32.15-second figure was the normal-RAM cycle subtotal converted
to seconds. The full profile includes another 18,129,242 TOS cycles and
530,418 cartridge cycles. Fast-forward does not invalidate these emulated
cycle counts.

Three interpretations in the supplied report need correction:

1. **`funlockfile` is not an expensive file operation here.** At address
   `0x3c973c` it is one `RTS`, executed 83 times for **1,434 cycles total**.
   The reported 18,661,094 equals those 1,434 plus all 18,659,660 TOS and
   cartridge cycles. A parser carrying the last program label into unlabeled
   ROM code reproduces the error exactly. At 32 MHz it costs 880 cycles in
   54 executions. Keep memory regions separate when aggregating profiles.
2. **`set256` is a local loop label inside `memset`, not a palette routine.**
   The disassembly at `0x3c1238` is a sequence of memory stores filling
   256-byte chunks. Its 38,000 executions are loop iterations, not 38,000
   function calls. Caller attribution is needed before deciding which buffer
   clearing to optimize.
3. **62.7% is not demonstrated usable audio headroom.** The quoted 335,266,498
   cycles exactly equal the sum of six selected timing/mixer symbol groups;
   subtracting them produces the quoted 199,149,498. These routines include
   necessary service work as well as repeated polling. Their aggregate cost
   cannot all be classified as removable. The corresponding remainder at
   32 MHz is 201,536,604, which supports approximately stable other work, but
   does not measure slack before each audio deadline.

The audio result makes the last distinction concrete. The instruction at
`0x1fde98` clears DMA playback control; the next instruction sets
`s_isrStoppedDma`. It executes 253 times at 16 MHz and twice at 32 MHz.
The corresponding recovery path in `update()` runs the same number of times.
[The source](../../../backends/mixer/atari/atari-mixer.cpp) takes this branch
when no update pulse occurred between audio interrupts. Actual long stalls
and spurious/closely spaced interrupt delivery need to be distinguished;
these counts alone do not measure audible dropouts. They do rule out using
unchanged game frame rate as evidence that audio scheduling is already sound.

Even accepting the report's headroom provisionally does **not** close PCM:

| Item | CPU time per 15.619 ms synthesis period |
| --- | ---: |
| Hypothetical 62.7% available time | **9.80 ms** |
| Two existing exact PCM partials | 7.82 ms |
| Three existing exact PCM partials | **11.73 ms** |
| Four existing exact PCM partials | 15.64 ms |
| Existing mixed stereo host-port upload, additional | 2.33 ms |

Three PCM partials already exceed that allowance with the upload removed.
Applying the earlier approximate mean demand of 3.5 to this renderer costs
13.69 ms, about 87.6% of the CPU, before controls or transport. Mean demand
is not a peak-capacity guarantee, either. A cheaper PCM path may change this,
but the reported idle percentage does not narrow the obstacle to DSP alone.

This strengthens the case for measuring a sample-based alternative, without
proving its 32-voice capacity. The next useful measurements are minimum slack
per audio period, due timer/mixer service costs, DMA-stop causes, speech and
moving actors, then the combined renderer. Memory high-water and physical
hardware validation remain open too.

## Recommended alternative: samples of whole instruments, played live

Record or render dry notes from the MT-32 patches, including each patch's
combined partials and attack. During play, the Falcon transposes, envelopes,
pans and mixes these samples in response to the game's live MIDI events.
Bank preparation can happen beforehand on the Falcon using the existing
offline renderer if all processing must remain on that machine; preparing
samples does not have a real-time deadline.

Proposed starting configuration:

- 32 mono sample voices with 16-bit sample data and stereo output at the
  Falcon's 24.585 kHz setting.
- Several key zones for important instruments; selected velocity layers
  where needed. Select one layer per note initially, rather than blending.
- Preserve attacks, use carefully chosen sustain loops, and implement live
  note-off, sustain, pitch bend, modulation, volume and pan.
- DSP interpolation, gain ramps and mixing, with bounded sample blocks from
  ST-RAM. The bank cannot reside entirely in the DSP's 32K-word SRAM.
- A shared inexpensive room effect, with its sound and cost measured. Avoid
  a filter and a separate reverb network for every sample voice.
- Start with a roughly 4 MiB bank ceiling; revise it after measuring actual
  game memory. Scope bank contents to Atlantis's 68 melodic programs and
  used rhythm sounds, prioritizing quality for the frequently used patches.
- Keep iMUSE's live sequencing and integrate behind one ScummVM audio owner.
  Instrument samples retain interactive transitions; fixed recordings of
  entire cues would need a different approach to transitions and overlaps.

This exchanges ongoing LA waveform computation for sample storage and
transport. The measured game average is 2.24 partials per sounding note, but
collapsing those into one sample voice is **not itself a measured 2.24x
speedup**: the two kinds of voice have different rendering and memory costs.

I expect a carefully prepared bank to preserve the recognizable attacks and
instrument character better than aggressive live LA simplification. This is
a sound-quality judgment to test by listening, not an audition result.
Samples will not reproduce arbitrary timbre SysEx, continuously changing
filter behavior, independent partial releases or velocity response exactly.
The resource analysis found factory patches rather than custom Roland timbre
uploads in Atlantis, which makes this particular game a better candidate.

Memory and bandwidth must be measured early. At 24.585 kHz, 32 mono 16-bit
streams at unity pitch already represent about **1.57 MB/s of sample payload**
before protocol overhead, interpolation margins and cache misses. Pitch
changes and refill patterns can increase that. A small inner DSP loop alone
does not establish feasibility.

## Existing Falcon software: useful references, with limits

**Graoumf Tracker / the GemGT2 replay engine** is the closest implementation
reference found. The published replay source has a 32-voice maximum, a
49,170 Hz base replay rate, 8/16-bit sample input, an interpolation path,
and host-port transfers of sample blocks. Its readme, however, reports
20 channels on a plain Falcon; its 32-channel stress case used a CT2a.
That supports investigating cheaper sample playback, but not guaranteeing
32 voices plus a game. Reducing output rate may help, but transport and CPU
costs need not scale with it.

Primary source: [GemGT2 release and included replay source](https://yescrew.atari.org/bin/graoumf/gemgt2.lzh),
specifically `GEMGT2/SOURCES/GT/README.TXT`, `PLAYDSP3.S` and `PLAYDSP.ASM`.
[The maintainer's release page](https://yescrew.atari.org/eng/products.htm#gemgt2)
identifies the Laurent de Soras routines and Earx optimizations. Source
availability alone does not establish permission to incorporate that code;
check its license before reuse.

**ACE MIDI** is an existing alternative synth, but it does not satisfy 32
voices on the stock machine. New Beat specifies up to 16 on a standard
Falcon and up to 28 on accelerated Falcons, with 16 MIDI parts. It is not
an MT-32-compatible ScummVM backend.
[Official ACE specifications](https://newbeat.atari.org/main.php?page=ace_specifications).

## The first experiment that can justify a 32-voice claim

Build a bounded sample-mixer prototype before investing in the full bank.
Measure 32 simultaneously sounding, independently pitched voices, including
short loops, high pitches and simultaneous refills. Include ramps, pan and
the chosen room effect. Budget 32 music voices plus the separate speech/effects
mix if 32 is to remain available to music during speech.

Measure complete CPU and DSP periods, sample integrity and underruns, first
under calibrated Hatari and then on the physical stock Falcon. Follow with
gameplay and overlapping cues using the same ScummVM-owned buffers. Compare
representative instruments and game transitions against Munt recordings.
Only passing those tests would establish the promised capacity and sound.

One correction matters when using the existing demand report: its 50.1%
figure for <=8 partials and 73.9% for <=16 include silent rendered time.
The report divides by `totalSeconds`, not `soundingSeconds`. They are also
aggregate partial-demand thresholds, not split CPU/DSP schedulability tests.
They must not be described as measured live coverage or used to promise that
doubling a pool solves the game's music.
