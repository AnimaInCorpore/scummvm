# OPL3 on the Falcon DSP: investigation

2026-09-16. Target: 16 MHz 68030, 32 MHz DSP56001, 14 MB, with ScummVM
running Fate of Atlantis. This note started as a source audit, a
reproduction of F030MXDRV measurements and a capture of the game's own OPL
register stream; it now also records the outcome.

**Status, 2026-09-22.** The measured exact DSP implementation exceeds the
budget; the practical OPL2 renderer runs at 49.17 kHz, with rhythm mode,
from the ScummVM build (`opl_driver=atari_dsp`). It now renders 48-frame
blocks, so a register write takes effect at most 0.98 ms early instead of
1.30 ms. Its kernel costs 58% of the budget on the Atlantis excerpt and 76%
in the nine-channel stress fixture, excluding production transport and SSI
overhead. Separate stream tests pass without late periods; the tightest
stress period leaves 2.17 ms. It preserves the AdLib arrangement and live
iMUSE behavior; nothing here has run on physical hardware. Atlantis passes
its game gate on the emulated Falcon with the 48-frame blocks; the other
game runs predate them.

Approximate OPL3 is a worthwhile experiment, but full OPL3 at the current
sample rate needs more than enabling eighteen channels. See
[Outcome](#outcome) for established results and
[Quality improvements and OPL3 roadmap](#quality-improvements-and-opl3-roadmap)
for the measured timing change and the proposed work. The exact benchmark
is evidence about this implementation, not proof that every exact
algorithm is beyond the DSP56001.

## What the games actually ask for

In this checkout, `ScummEngine::setupMusic()` enables `PROP_SCUMM_OPL3`
only for `GID_SAMNMAX`. Atlantis does not request OPL3. Choosing a capable
OPL emulator does not change which arrangement the game feeds it.

| Path in the current source | Logical note slots | Synthesis work at full allocation |
| --- | ---: | --- |
| Atlantis AdLib | 9 | 9 two-operator channels, 18 operators |
| Sam & Max OPL3 | 9 | two layered two-operator channels per slot, 36 operators |
| General OPL3 chip | depends on channel configuration | up to 18 two-operator channels, or combinations with six possible four-operator pairs |

`MidiDriver_ADLIB::_voices` has nine entries in both modes. Sam & Max loads
two instrument definitions, writes the same note into both register banks,
and keys both off together. Its primary layer enables both stereo outputs;
the secondary chooses left or right from the part's pan. This is layering,
not eighteen independently allocated MIDI notes, and not the chip's hardware
four-operator mode. The driver does not enable register `0x104`'s four-operator
pairs. It issues `0xBD = 0` at initialization, a write its own zeroed register
cache absorbs before the chip sees it, and uses melodic FM channels for its
percussion instruments rather than enabling hardware rhythm mode.
These are properties of this driver, not limits on OPL3 generally.

Sources: [game selection](../../../engines/scumm/scumm.cpp),
[voice allocation, instrument tables and register writes](../../../audio/adlib.cpp),
and the primary [Nuked-OPL3 implementation](https://github.com/nukeykt/Nuked-OPL3/blob/master/opl3.c).

### Atlantis resource inventory

The new [resource audit](../tools/opl-resource-audit.py) resolves sound IDs
through DSOU/LOFF and parses complete SMFs with the existing FCM reader.
It does not scan for tag signatures or pretend raw MIDI is live iMUSE.

| Item | Measured count |
| --- | ---: |
| AdLib alternatives | 200 |
| AdLib SMF tracks | 253 |
| AdLib stored events | 175,779 |
| Part instrument-definition events (`0x7D 0x10`) | 1,107 |
| Global instrument-definition events (`0x7D 0x11`) | 17 |
| Distinct decoded AdLib instrument definitions | 254 |
| AdLib resources containing definitions | 200 |
| Roland alternatives | 204 |
| Sound Blaster digital alternatives | 3 |

Roland sound IDs 60, 118, 202 and 203 have no AdLib alternative. The normal
resource-selection rules must remain authoritative; a backend should not
feed their Roland data through an FM instrument mapper. `readSoundResource()`
already explicitly documents the issue for Atlantis sound 60.

The 254 definitions include driver modulation/envelope parameters; they are
not a measured set of 254 perceptually distinct sounds. Event counts include
stored branches/tracks, not an execution trace. The register bandwidth and
simultaneous voice demand they cannot establish are measured separately in the
next section. All 204 Roland payloads were also cross-checked byte for byte
against the existing indexed FCM extractor.

**Forcing Sam & Max's OPL3 property on Atlantis is incorrect.**
`AdLibPart::sysEx_customInstrument()` and the percussion override reject
custom instruments in OPL3 mode, and that mode also changes software
modulation, volume and pitch handling. All 200 AdLib resources contain
instrument definitions. Keep Atlantis's existing AdLib driver semantics and
implement the chip underneath it. Any enhanced eighteen-note or layered
Atlantis arrangement would be a separate, intentionally changed renderer.

## Measured register stream

Implementation step 1 is now done for Atlantis. The new
[capture harness](../tools/foa-opl3/README.md) links a separate headless
executable in which only the OPL factory is replaced, so the real game,
resource selection, iMUSE and `audio/adlib.cpp` run unchanged and what is
recorded is what that driver already decided to send, after its own register
cache dropped redundant writes. Callbacks are driven at the 250 Hz the driver
asks for, not at `Audio::RealChip`'s 100 Hz cap, and the null backend's
existing virtual test clock makes repeated runs identical.

Each window below is two gated runs that produced byte-identical traces.
Both are the unattended opening with speech muted and no player input, and
both start game sounds 150, 21, 22, 29 and 30, matching the MT-32 reference
trace's cue sequence.

| Measurement | 60 s window | 300 s window |
| --- | ---: | ---: |
| Register writes | 12,247 | 52,356 |
| Mean writes/s | 204.05 | 174.51 |
| Driver callbacks, all on the 4,000 µs grid | 15,002 | 75,001 |
| Writes from a driver callback | 12,234 | 52,345 |
| Writes from the engine thread | 13 | 11 |
| Callbacks carrying at least one write | 1,748 | 7,372 |
| Writes in the busiest single callback | 129 | 129 |
| Peak writes in any 100 ms | 147 | 147 |
| Peak writes in any 1 s | 489 | 489 |
| Peak simultaneously keyed channels | 9 | 9 |
| Mean keyed channels at callbacks | 4.571 | 5.049 |
| Key-on edges | 992 | 4,830 |
| Key-ons on an already-keyed channel | 13 | 13 |
| Second-bank and `0xBD` writes | 0 | 0 |

The extra 240 seconds add no new cue and no denser passage; every peak above
is set inside the first 60 seconds. These are measured occupancy and traffic
for these runs, not a worst case for the whole game.

Four results change how a backend should be built:

- **Traffic is bursty and tick-aligned.** The busiest callback carries 129
  writes while most callbacks carry none. Sizing a host-port transport on the
  204 writes/s mean would understate that burst by about 160 times. A small
  number of writes are not tick-aligned, so ordering cannot be assumed to
  follow tick boundaries.
- **Nine voices are genuinely occupied**, averaging about five. Only 13
  key-ons land on a channel that is still keyed, so voice reuse happens but is
  not the common case in this material.
- **The driver forwards raw instrument bytes.** 3,991 of the 6,789 waveform
  select writes carry bits a YM3812 ignores, with raw values reaching `0x7e`,
  because `adlibSetupChannel()` writes `instr->modWaveformSelect` unmasked. A
  backend must mask to the chip's significant bits rather than reject or trust
  the value. All four OPL2 waveforms are used after masking.
- **`0xBD` never reaches the chip.** The driver's `adlibWrite(0xBD, 0)` at
  open is absorbed by its own zeroed register cache, and only `0x08 = 0x40`
  and `0x01 = 0x20` are sent as chip control. A backend must come up with
  rhythm mode off and AM/vibrato depth zero on its own.

Nothing in the kernel can be scoped out: tremolo, vibrato, the sustaining
envelope flag and key-scale rate are all set by real instruments, feedback
takes every value from 0 to 7, and both connection types appear. Four-operator
mode, the second register bank and rhythm mode are never touched.

**This is a register stream, not a cost.** No audio was rendered, no OPL
emulator was reached, and no synthesis, transport or deadline was timed.
Register counts are not instruction counts.

## What F030MXDRV establishes

The sibling project implements the **YM2151/OPM**, not OPL3. Its practical
renderer runs eight four-operator channels at 32,779.9479 Hz. It uses the
DSP's 256-step sine ROM, fixed-point block rendering, block-rate envelopes
and LFO work, and ordered register-event handling. It has a separate exact
62.5 kHz implementation for conformance. Its practical path targets perceptual
compatibility, not sample equality to the chip.

I ran `make profile-dsp-rt5` (including its `check` prerequisite), then
profiled Xevious and STAGE5 with `tools/profile_dsp_live.py`. These fresh runs
reproduce the documented figures at F030MXDRV commit `ad9ef69`:

| Measurement | DSP instruction cycles/output frame | Interpretation |
| --- | ---: | --- |
| Available at 32.780 kHz | 489.40 | calibrated Hatari's 16.042494 MIPS |
| 8,192-frame support/synthesis fixture | 336.60 | bracket excludes production host receive, SSI ISR and refill dispatch |
| Xevious live window: synthesis and transport | 391.80 | 128 rendered payloads after 300 warm-up refills |
| Xevious including host-port stalls | 392.24 | 80.1%; window/nominal payload duration 1.000x |
| STAGE5 including host-port stalls | 422.55 | 86.3% average; window/nominal payload duration 1.031x |

STAGE5 demonstrates why a below-budget average is insufficient. The elapsed
profile window exceeds the nominal duration of its 128 rendered payloads.
The project's detailed timing report additionally records missed boundaries
and envelope/register bursts exceeding the budget. This investigation did
not freshly count individual late handoffs, so it does not claim a new
late-buffer count from the aggregate profile.

The README's broad claim that dense FM songs fit needs that qualification.
The detailed report records 400–430 cycles/frame typically and 480–517 at
peaks before the final vibrato correction; that correction raises STAGE5's
typical cost to about 422 and its recorded misses to 40. Those historical
per-period figures were not remeasured here.

There is useful physical-hardware evidence too: the project documents
external-memory timing/aliasing tests, a passing codec-rate measurement, and
good-sounding Xevious playback after clearing the DSP Bus Control Register.
It also documents an earlier intermittent clock-start failure. The listening
session was uninstrumented, the latest dense-song changes had not been
retested on hardware, and it is not ScummVM gameplay qualification.

Sources: [detailed timing and hardware observations](/Users/saschaspringer/Work/F030MXDRV/docs/hatari-timing.md),
[practical kernel](/Users/saschaspringer/Work/F030MXDRV/src/dsp/ym2151.asm),
[quality contract](/Users/saschaspringer/Work/F030MXDRV/docs/perceptual-compatibility.md).

### What can transfer

| F030MXDRV mechanism | Use for this project |
| --- | --- |
| Boot loader and explicit `BCR = 0` | Avoid the measured fifteen external-memory wait states after boot; preserve sound ownership and restore machine state |
| Internal-memory hot loops, X/Y layout and parallel moves | Reuse optimization techniques; create a new audited memory map for 36 operators and OPL waveforms |
| Phase accumulation, modulation rings, fixed-point mixing | Reuse the rendering structure, with OPL-specific phase, gain and feedback conventions |
| Cached register decoding and active-envelope lists | Move invariant work out of sample loops; bound simultaneous key-on and instrument-load costs |
| Timestamped events and event-aligned block splits | Preserve register ordering and audio-clock timing |
| READY handshake, early receive, producer lookahead | Avoid host-port latch loss and deadline stalls |
| SSI output and separate exact/reference captures | Reuse the validation approach and output machinery through one Falcon audio owner |

The YM2151 register map, envelope coefficients, key-code/detune tables,
algorithms, LFO and noise model cannot simply be relabelled OPL3. OPL needs
F-number/block pitch, OPL multiplier and KSR/KSL rules, its sustain behavior,
four/eight selectable waveforms, OPL tremolo/vibrato and feedback scaling.
A complete generic OPL3 implementation also needs four-operator pairing,
hardware rhythm/noise and mode-switch behavior. Scope unsupported features
explicitly until implemented; do not advertise a general OPL3 backend from
a Sam & Max-only subset.

## Kernel foundations and measured cost

The exact
[kernel](../tools/foa-opl3/README.md#the-synthesis-kernel) is written, checked
against a chip model, transliterated to DSP56001 assembly and benchmarked.
The host reference includes envelopes, LFOs and rhythm mode; the measured
exact DSP synthesis loop does not, so its cost below is a lower bound for
completing that implementation.

`opl-kernel.h` implements two-operator OPL synthesis in the arithmetic a
DSP56001 executes directly, and is **bit exact against Nuked-OPL3** over
19,303,934 samples and 58,823 register writes in the committed
[kernel results](../tools/foa-opl3/kernel-results.json): parameter sweeps
covering every multiplier, envelope rate, key-scale setting, waveform, connection, feedback
depth and LFO depth, nine- and eighteen-channel polyphony with key cycling and
mid-note patch reloads, rhythm mode, and captured Atlantis and Cruise for a
Corpse streams. OPL2 waveform-enable behavior is tested with the waveforms
gated before they reach the OPL3 oracle. This is software-reference equality
at the chip's native rate, not a comparison with recorded Yamaha hardware.

Four results change the picture this note started from:

- **The waveform ROM is 512 words, not 8,192.** All 8,192 entries of the eight
  unpacked 1,024-entry tables reproduce exactly from a 256-entry quarter
  log-sine table plus index and sign arithmetic. With the 256-entry
  exponential ROM that is exactly the DSP56001's internal X and Y data RAM,
  so the external waveform reservation estimated above is not needed and the
  hot loop needs no external table access. Both ROMs are closed-form and are
  generated rather than copied.
- **External memory costs nothing extra once the bus is configured.**
  F030MXDRV measured the Falcon's DSP SRAM at zero wait states on all three
  external paths after clearing the bus control register, which reset leaves
  at fifteen wait states on the `Dsp_ExecBoot` path. Placement still matters
  for the emulator's two-cycle charge when one instruction reaches two
  external spaces.
- **An exact OPL envelope cannot be held constant across a block.** It
  advances on the chip's sample clock under a shared counter. The measured
  exact renderer uses a frame-major loop; the practical renderer amortizes
  control work by holding it constant within each operator-major block.
  Exact block processing is possible in principle with per-sample control
  values and preserved dependencies, but its arithmetic, storage and memory
  traffic have not been implemented or measured here.
- **The 18-operator and 36-operator paths share one implementation**, so the
  eighteen-channel benchmark the recommendation asks for is a configuration
  of the same kernel rather than separate work.

### Measured synthesis cost

The kernel's synthesis loop is now implemented in DSP56001 assembly and
measured on an emulated Falcon. Both configurations render output word for
word identical to the host reference, so the loop being timed is the real
algorithm and not a stand-in.

| | 9 channels, 18 operators | 18 channels, 36 operators |
| --- | ---: | ---: |
| Mismatches against the host kernel | 0 | 0 |
| DSP instruction cycles per frame | 1,025.00 | 1,983.00 |
| Instruction cycles per operator | 56.94 | 55.08 |
| Share of the 24.585 kHz budget | 157% | 304% |
| Share of the 32.780 kHz budget | 209% | 405% |
| Share of the 49.170 kHz budget | 314% | 608% |

**At 32.78 kHz, synthesis alone costs about twice the budget for Atlantis's
nine-channel arrangement, and the envelope generator is not in that number.** These runs
hold every envelope constant; tremolo, vibrato, DSP-side register decoding,
SSI output and the host transport are all absent as well. Every one of them
adds to the figure and none subtracts.

In this implementation, cost scales almost linearly with operators — 56.94
against 55.08 cycles per operator across a doubling of load. The loop uses
no parallel X/Y moves, which is the obvious remaining optimization; counting
the move and ALU pairs that could fuse suggests roughly 40-45 cycles per
operator, an estimate from reading the code rather than a measurement, and
one that would still leave nine channels over budget at 32.780 kHz before the
envelope is added. The synthesis loop alone also occupies 495 of the 512
internal program words `Dsp_ExecBoot` can load, so a complete kernel needs
external program memory or a two-stage loader.

These measurements do not establish a minimum cost for all exact renderers.
They establish that this frame-major synthesis loop is already too costly
before per-sample envelopes and the rest of the chip are added. Reorganizing
exact control into blocks would require a new implementation and benchmark.

Every cycle figure is Hatari's model, which charges Falcon external memory
zero wait states and which Hatari's own documentation calls instruction-wise
correct rather than cycle accurate. Nothing here ran on hardware, no audio was
auditioned, and no output-rate conversion is modelled.

## Budget implications

The same calibrated instruction rate provides these budgets:

| Synthesis rate | Cycles per synthesis frame |
| --- | ---: |
| 24,584.96 Hz | 652.53 |
| 32,779.95 Hz | 489.40 |
| 49,169.92 Hz | 326.27 |
| approximately 49,716 Hz OPL native rate | 322.68 |

The practical prototype started at 32.780 kHz and moved to 49.170 kHz after
the lower rate produced audible aliasing. Phase, envelopes and LFO clocks
are retimed; merely changing the output divider is wrong. Native-rate
synthesis plus resampling remains a separate experiment. Codec-rate
synthesis changes aliasing and feedback behavior and must be assessed
against an OPL reference.

OPL3 has only 12.5% more operators than OPM, but that does not imply a 12.5%
cost increase. Eighteen two-operator channels can require **eighteen feedback
paths instead of eight**, more channel dispatch, and waveform lookup costs
different from the internal sine ROM. Envelope bursts are already a problem
in MXDRV. Even an unjustified uniform 36/32 scaling would turn the measured
392.24–422.55 averages into 441.27–475.37 cycles/frame, leaving little room
for those differences. This arithmetic is sensitivity analysis, not an OPL3
performance prediction.

Atlantis's 18-operator mode has a materially smaller synthesis workload.
It still needs all driver modulation, releases, custom definitions and
original voice-allocation behavior. The DSP should implement the allocated
hardware voices, not steal additional notes to meet an average budget.

OPL avoids the MT-32's large PCM-ROM access problem and needs no sampled
instrument bank. Its waveforms can be generated algorithmically. For scale,
eight unpacked 1,024-entry tables occupy 8,192 DSP words (24 KiB), out of
32,768 shared external words. That is a possible table representation,
not a completed memory layout; program, state and buffers share that SRAM,
and external lookups have timing consequences. A smaller waveform table
requires its own quality check. No MT-32-style reverb is needed to reproduce
the original OPL arrangement.

## ScummVM integration

The production `OPL::OPL` backend below `writeReg(int,int)` keeps iMUSE and
the AdLib instrument/voice driver on the 68030. Its decoder converts
register writes into DSP parameter events at block boundaries, preserving
key-on edge counts. The Roland-specific FCM package is not substituted for
ADL resources.

- **Factory selection:** `audio/fmopl.cpp` advertises `atari_dsp` as OPL2
  only. A future OPL3 path also needs the `ENABLE_OPL3` build condition in
  `audio/adlib.cpp` to recognize the DSP backend after its required features
  are implemented and qualified.
- **Audio-clock callbacks:** the driver's 250 Hz callbacks advance on a
  fractional sample timeline during period production, and each callback's
  writes land in the 48-frame block that contains its time. Timer A handles
  delivery and starts production only outside the tracked critical
  sections. Extension periods keep synthesis running when callbacks cannot
  run; each postpones the sequencer by one 15.62 ms period.
- **Audio ownership:** the DSP sends FM plus host-mixed mono speech/effects
  through SSI. The main loop prepares PCM ahead; a sufficiently long loop
  stall can empty that ring while FM continues. `atari_dsp_audio=false`
  restores the DMA path.
- **State:** reset, pause and volume handling are implemented, with focused
  gates described in the [integration notes](../tools/foa-opl3/README.md#in-the-game).
  Broader gameplay, save/load and interrupt-safety qualification remain open.

Sources: [OPL interface](../../../audio/fmopl.h),
[callback implementations](../../../audio/chip.cpp),
[Falcon build](../../../backends/platform/atari/build-falcon030.sh),
[audio ownership](../../../backends/mixer/atari/atari-mixer.cpp).

## Implementation sequence and acceptance criteria

1. Capture timestamped writes **after the real AdLib driver** for Atlantis.
   Done for the unattended opening, including scene changes, overlapping cues,
   the driver's own modulation writes and the chip-control writes that survive
   its register cache; burst sizes and keyed-voice occupancy are in the
   measured stream above. Still open: player-driven gameplay, menus, save/load
   and MIDI-driven effects outside that sequence, where voice stealing should
   be more frequent than the 13 retriggers seen here. Sam & Max needs its own
   capture when its data is available. The shipped C++ Nuked implementation is
   suitable as a development-machine reference; verify OPL2-mode behavior
   separately.
2. Build a DSP two-operator kernel with OPL pitch, waveform selection,
   envelope, feedback and AM/vibrato semantics. Done with the practical
   OPL2 kernel, including rhythm mode. The exact DSP synthesis experiment
   remains over budget and incomplete; its host reference supplies the
   quality oracle for the practical path.
3. Compare captured DSP audio and register timing with the reference at the
   same output rate. Check pitch, attack/release, low-level envelopes,
   vibrato/tremolo, feedback spectra and transients, then audition actual
   music and effects. Reduced sample rate alone is not a quality verdict.
4. Integrate the nine-channel Atlantis mode first and measure deadline
   margins while speech, animation, room loads and user interactions run.
   Require no missed or repeated periods, preserved event ordering, and
   measured CPU/DSP worst-period headroom. A proposed engineering target is
   20% worst-period DSP margin; that is a target, not an achieved result.
5. Qualify the paired-channel Sam & Max path, then generic four-operator
   mode if desired. OPL2 rhythm mode is already implemented and tested in
   emulation. Repeat on physical hardware with counters and sustained
   gameplay before declaring the target complete.

**Benchmark decision, 2026-09-16.** The original recommendation was to
qualify the OPL2-compatible Atlantis path first. The measured exact loop
costs 209% of the 32.780 kHz budget with the envelope generator still absent;
the estimated parallel-move optimization would not close that gap. This
motivated the practical renderer, rather than establishing a hardware
impossibility for all exact implementations.

What remained open was an *approximate* renderer, along the lines F030MXDRV
already took for the YM2151: block-rate envelopes and LFO, a lower synthesis
rate with the clocks correctly retimed, and perceptual rather than sample
compatibility. That renderer was then built and measured; the next section
records it. None of these findings make FM timbres sound like an MT-32.

## Outcome

The practical kernel, its DSP implementation, the stream transport and the
ScummVM integration are in [tools/foa-opl3](../tools/foa-opl3/README.md),
each with a gate and a committed result file:

| Measurement | Result |
| --- | ---: |
| Practical kernel against the exact one, sustained-tone fixture | aligned envelope error at most 0.03 dB, partial error at most 0.635 dB, pitch error 0.56 cent |
| Aligned envelope correlation, Atlantis 60 s | 0.9862, mean level error 0.607 dB, measured onset skew at most 2.77 ms (0.9825, 0.681 dB and 2.83 ms with 64-frame blocks) |
| Write timing, Atlantis 60 s | key-ons take effect a mean 0.53 ms and at most 0.98 ms early (0.63 and 1.30 ms with 64-frame blocks) |
| Band levels against the exact kernel, Atlantis 60 s, a window every 0.5 s | mean absolute error 0.61 dB below 3 kHz, 0.65 dB at 3-8 kHz, 1.07 dB at 8-15 kHz (0.69, 0.81, 1.09 dB with 64-frame blocks) |
| Aligned envelope correlation, Cruise 60 s with rhythm mode | 0.9968, mean level error 0.106 dB (0.9972 and 0.099 dB with 64-frame blocks) |
| Aliasing: energy above 3 kHz against the exact kernel, Atlantis 60 s | +0.9 dB at 49.17 kHz (+2.7 dB at 32.78 kHz, heard as blurred instruments), with 64-frame blocks |
| DSP cost at 49.17 kHz, Atlantis first 4 s | 189.7 cycles per frame, 58% of budget, word exact (175.7 with 64-frame blocks) |
| DSP cost at 49.17 kHz, nine feedback FM channels with LFO held | 248.4 cycles per frame, 76% of budget, word exact (232.9 with 64-frame blocks) |
| Stream mode through the SSI | Atlantis 20 s: 1,280 periods; nine-channel stress fixture 10 s: 640 periods; none late, checksums equal; the tightest period left 3.29 ms (Atlantis) and 2.17 ms (stress) of its 15.62 ms |
| Atlantis, Tentacle, Monkey Island 1 and 2, Cruise on the emulated Falcon | opening music present, no late periods or protocol errors; Atlantis with 48-frame blocks, the others with 64; per-game counters and limitations in the [integration notes](../tools/foa-opl3/README.md#in-the-game) |

The practical kernel and integration pass their bounded emulation gates;
listening, wider gameplay coverage, the worst-period margin inside a game
and physical hardware remain unqualified. The correlations above compare
aligned 20 ms RMS loudness envelopes, not audio sample waveforms or a
percentage of perceptual accuracy. Kernel costs exclude production host
transport and SSI overhead; the separate stream tests establish deadlines
for their tested workloads, not a universal spare-capacity percentage.

Production and delivery both run from a Timer A interrupt, the main
loop only mixes PCM ahead, and the game's stalls at scene changes (seconds,
as the transport's PCM underrun count shows) no longer touch the music;
extension periods cover the stretches where the interrupt may not run
iMUSE, at the cost of 15.62 ms of sequencer slip each. A listening pass must
include those slips as well as FM timbre and transients.

## Quality improvements and OPL3 roadmap

2026-09-22 assessment of the existing code and committed measurements.
The control-timing change below is implemented and measured; the other
experiments have not been implemented or benchmarked. Evaluate an
eighteen-channel prototype separately, and keep the 49.17 kHz OPL2 path as
the comparison baseline.

### Control timing: 48-frame blocks

Envelopes and LFOs advance once per block, and a register write takes
effect at the start of the block that contains its timestamp, up to one
block early. The block was 64 frames (1.30 ms) and is now 48 (0.98 ms).
This quantization is separate from output buffering and from the 15.62 ms
sequencer slip caused by an extension period; shorter synthesis blocks do
not cure either of those transport effects.

The output period stays 768 frames with host PCM at 192 samples, and the
fractional callback clock in
[`AtariDspOPL::producePeriod`](../../../backends/platform/atari/dsp-opl.cpp)
is unchanged: it maps each callback's sample position to a block, so the
driver's tempo does not depend on the block length.
[`generate-tables.py`](../tools/foa-opl3/generate-tables.py) derives the
period's block count from its frames and regenerates the retimed envelope
and LFO tables, and the assertions in `atari-dsp.cpp` hold
[`atari-dsp.h`](../../../backends/platform/atari/atari-dsp.h) to them.
Three block lengths were measured with the same gates, the kernel and
stream ones on the emulated Falcon:

| Frames per block | 64 | **48, adopted** | 32 |
| --- | ---: | ---: | ---: |
| Control interval, blocks per period | 1.30 ms, 12 | 0.98 ms, 16 | 0.65 ms, 24 |
| Atlantis key-on lead, mean / max | 0.63 / 1.30 ms | 0.53 / 0.98 ms | 0.33 / 0.65 ms |
| Onset skew, feedback, high-pitch and rhythm scenarios, max | 1.29 ms | 0.98 ms | 0.65 ms |
| Atlantis aligned envelope correlation, mean level error | 0.9825, 0.68 dB | 0.9862, 0.61 dB | 0.9838, 0.71 dB |
| Atlantis band levels, mean absolute error below 3 kHz / 3-8 kHz / 8-15 kHz | 0.69 / 0.81 / 1.09 dB | 0.61 / 0.65 / 1.07 dB | 0.64 / 0.67 / 1.02 dB |
| DSP cycles per frame, stress / Atlantis | 232.9 / 175.7 | 248.4 / 189.7 | 279.4 / 217.6 |
| Share of the budget, stress / Atlantis | 71% / 54% | 76% / 58% | 86% / 67% |
| Tightest stream period's slack, stress / rhythm / Atlantis | 2.95 / 3.03 / 4.08 ms | 2.17 / 2.17 / 3.29 ms | 0.60 / 0.46 / 1.72 ms |

The timing figures follow the block: a write's lead, and with it the onset
skew of fast attacks, shrinks with it. The Atlantis mix improves from 64 to
48 frames but not consistently from 48 to 32. What remains there behaves
like partials of different channels summing with phases the timing shifts
rather than like the lead itself, though this comparison does not isolate
the two. The onset skew of the slow-attack scenario
(2.66, 2.39 and 2.72 ms) is the chip starting an attack on its next rate
tick. In the per-note scenarios the aligned errors stay within their
thresholds at every length; the slow attacks of the envelope scenario grade
slightly worse at 48 frames (partials 1.52 against 1.42 dB mean).

The cost is per-block work: the operator boundary pass, the block and
channel boundary, the loaders and the render driver grow with the number of
blocks, while the per-frame stages barely move (183.8, 185.1 and
187.6 cycles/frame in the stress case). The stream gate now also reports
the least time any render left before the transmitter reached its half,
host transport included. Its host submits as fast as the DSP takes
periods; the game's transport answers READY only on its next 1 kHz tick,
up to a millisecond later, and at 32 frames the tightest stress and rhythm
periods leave about half a millisecond. 48 frames leaves 2.17 ms in the
worst case and 3.29 ms in the Atlantis stream. Neither length meets the
20% worst-period target with the stress fixture (18.9% at 64 frames, 13.9%
at 48). An older 48-frame implementation overran after transport overhead,
before the boundary-pass rewrite, at 88% of the budget in the stress case.

In the game, Atlantis's opening streams 5,823 periods with 48-frame blocks,
twice with the same counters: no late period, no protocol error, 14
extension periods. The Tentacle, Monkey Island and Cruise for a Corpse runs
in the integration notes were made with 64-frame blocks, and the tightest
in-game deadline is not measured at all.

Event-aligned splits are a later alternative: carry sample offsets, render
up to a write, apply it, then render the remainder in order. This requires
variable-length loops, duration-dependent envelope/LFO advancement and a
bound on split overhead during register bursts. Group simultaneous writes
without losing key edges. More frequent envelope updates around fast attacks
could also preserve long blocks for steady notes. Merely interpolating the
final mixed output cannot repair early key-ons or an incorrect modulator
envelope; any gain interpolation must be evaluated within the FM chain.

### Match the chip clock and rhythm noise more closely

Native-rate synthesis near 49,716 Hz would add about 1.1% to the number of
FM synthesis frames relative to 49,169.92 Hz, plus the separate cost of a
resampler. Prototype it with the practical kernel, preserving the native
phase increments and retiming its controls, then convert to the codec rate.
Measure the resampler's filtering, buffering and cycles. This can address
shifted aliases and percussion phase artifacts; approximate envelopes and
other arithmetic differences would remain. A lower output rate alone is
not equivalent to native-rate synthesis followed by filtered conversion.

For rhythm noise, investigate a lookup/XOR jump-ahead transformation of the
23-bit linear-feedback shift register. It may reproduce multiple chip
steps without executing 36 individual shifts per sample. Verify the seed,
state convention and the bits read at each drum's chip slot against the
exact reference. Matching the state advance alone is insufficient when
synthesis clocks or phase-bit sampling differ. No cost or audible benefit
has been established for this candidate.

### OPL3 features and full-polyphony cost

The practical decoder and records have room for eighteen channels, but
only four waveform tables are loaded: selections 4-7 alias to 0-3 through
`selected % kWaveforms`. Channel output-enable bits are ignored, the SSI
writer duplicates the mono mix, and four-operator pairing is absent. A
complete OPL3 path needs eight waveforms, stereo routing, mode-switch
semantics and four-operator algorithms, plus a revised memory layout and
factory/build integration. See the
[implementation scope](../tools/foa-opl3/README.md#current-opl2-and-opl3-scope).

The existing stages alone extrapolate from 185.1 to **370.1 cycles/frame**
when nine fully active feedback FM channels become eighteen. That exceeds
the entire **326.27-cycle** budget at 49.17 kHz before control, stereo
mixing, transport or SSI. This is a scaling estimate for the current loops,
not a measured eighteen-channel practical benchmark. Full-rate OPL3 needs
substantial optimization or another rendering strategy. Four-operator
pairing connects the same pool of 36 operators; it does not double that
pool again, although routing and algorithms need implementation and timing.

At 32.78 kHz the budget is 489.40 cycles/frame, making an approximate
eighteen-channel prototype plausible but tight. That is not established
capacity, and the earlier OPL2 version's audible aliasing makes it a quality
tradeoff. Silent-channel skipping is already implemented and cannot be
counted as a new worst-case optimization.

Sam & Max's layered two-operator path is the first useful ScummVM OPL3
target: it needs both banks and stereo, but no hardware four-operator mode.
Capture its actual waveform usage and event bursts when data is available,
then test all eighteen channels active as well as the game trace. OPL3
support does not upgrade Atlantis's source arrangement; preserve its
existing AdLib driver behavior.

### Acceptance for each experiment

Keep separate evidence for host-reference quality, DSP word equality to
that practical reference, and complete-stream deadlines. Reuse the unit,
practical, DSP bench and stream gates, extending them for new timing or
OPL3 semantics. Exercise dense parameter bursts, fast attacks, all feedback
depths, rhythm, pause/reset and full polyphony; run games with PCM and
resource loads. Measure worst-period cost and transport waits rather than
spending the apparent 24% kernel margin without accounting for overhead:
the stream gate's slack is that measurement for its own host, and the
stress fixture's tightest period leaves 13.9% of the period against that
margin. The 20% worst-period margin above remains an engineering target.

Audition matched-level reference/DSP captures and check physical Falcon
timing before claiming Yamaha-level authenticity. The DSP56001's arithmetic
is suitable (24-bit multiplication and 56-bit accumulators); execution time
and memory placement are the immediate constraints. Its
[Motorola datasheet](https://www.nxp.com/docs/en/data-sheet/DSP56001.pdf)
specifies 16.5 MIPS at 33 MHz, consistent with the Falcon benchmark's
two-clock instruction-cycle budget. No comparison against a CQM-based
Sound Blaster has been measured here.

## Evidence and reproduction

Fresh reports, raw DSP profiles, game hashes and binary/source hashes are in
`build-falcon030/opl3-investigation/` (ignored generated artifacts).
`provenance.json` identifies the inspected sources and executed binaries.
The capture harness's own gated result, with source, binary and game hashes,
is in [its results.json](../tools/foa-opl3/results.json). The capture stage
does not change the production backend or game configuration: it replaces
the OPL factory only inside a separate headless executable built in its own tree.

From this repository:

```sh
python3 devtools/atari-falcon030/tools/opl-resource-audit.py \
  --game /path/to/atlantis-cd \
  --output build-falcon030/opl3-investigation/atlantis-resources.json

sh devtools/atari-falcon030/tools/foa-opl3/build-capture.sh
python3 devtools/atari-falcon030/tools/foa-opl3/capture-gate.py \
  --game /path/to/atlantis-cd --output build-falcon030/opl3-capture
python3 devtools/atari-falcon030/tools/foa-opl3/test-analyze-opl.py

make -C devtools/atari-falcon030/tools/foa-opl3/build/headless \
  -f Makefile -f ../../kernel.mk opl-kernel-test
python3 devtools/atari-falcon030/tools/foa-opl3/kernel-gate.py \
  --trace build-falcon030/opl3-capture/run-a/opl-writes.ev \
  --output build-falcon030/opl3-kernel
```

From F030MXDRV, using its DSP-calibrated Hatari default:

```sh
make profile-dsp-rt5
python3 tools/profile_dsp_live.py --song XEVIOUS \
  --output build/opl3-audit-xevious.txt --raw-profile build/opl3-audit-xevious.profile
python3 tools/profile_dsp_live.py --song STAGE5 --corpus-dir tests \
  --output build/opl3-audit-stage5.txt --raw-profile build/opl3-audit-stage5.profile
```

The live commands profile 128 refills after 300 warm-up refills. These are
bounded YM2151 evidence windows; no OPL3 capacity, entire-song cadence,
ScummVM integration or new physical-hardware result is implied.
