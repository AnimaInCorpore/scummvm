# OPL3 on the Falcon DSP: investigation

2026-09-16. Target: 16 MHz 68030, 32 MHz DSP56001, 14 MB, with ScummVM
running Fate of Atlantis. This note started as a source audit, a
reproduction of F030MXDRV measurements and a capture of the game's own OPL
register stream; it now also records the outcome.

**Outcome, 2026-09-16 (see [Outcome](#outcome) at the end).** An exact OPL
renderer does not fit the DSP, but a practical block-rate one does. Since
2026-09-17 it runs at the codec's 49.17 kHz, next to the chip's own rate,
because at 32.78 kHz its aliasing was audible: measured at 62% of that
budget on the Atlantis stream and 80% with nine feedback FM channels held
with tremolo and vibrato (55% and 69% at 32.78 kHz), word exact against its
host reference, streaming through the SSI without a late period, and
playing the game's opening on the emulated Falcon from the ScummVM build
(`opl_driver=atari_dsp`). It preserves the AdLib arrangement and live
iMUSE behavior; it does not reproduce the MT-32 arrangement or its
instrument sound, and nothing has run on hardware.

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

Step 2 is done for synthesis. The
[kernel](../tools/foa-opl3/README.md#the-synthesis-kernel) is written, checked
against a chip model, transliterated to DSP56001 assembly and benchmarked.
Its envelope generator is not implemented, so the cost below is a lower bound.

`opl-kernel.h` implements two-operator OPL synthesis in the arithmetic a
DSP56001 executes directly, and is **bit exact against Nuked-OPL3** over
4,189,988 samples and 47,846 register writes: parameter sweeps covering every
multiplier, envelope rate, key-scale setting, waveform, connection, feedback
depth and LFO depth, nine- and eighteen-channel polyphony with key cycling and
mid-note patch reloads, and the captured 60-second Atlantis stream replayed at
its recorded times. Nuked is a chip model, so the bar is sample equality.

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
- **An exact OPL envelope forbids block rendering.** The envelope generator
  advances every sample, driven by a chip-wide counter, so the operator-major
  block loop that makes F030MXDRV's YM2151 kernel affordable is not available
  without approximating it. A faithful OPL kernel is frame-major, and that is
  now the main open cost question rather than table memory.
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

**Synthesis alone costs about twice the budget for Atlantis's own nine-channel
arrangement, and the envelope generator is not in that number.** These runs
hold every envelope constant; tremolo, vibrato, DSP-side register decoding,
SSI output and the host transport are all absent as well. Every one of them
adds to the figure and none subtracts.

Cost scales with operators rather than channels — 56.94 against 55.08 cycles
per operator across a doubling of load — so the 36-operator target is simply
twice the nine-channel cost, with no economy of scale to find. The loop uses
no parallel X/Y moves, which is the obvious remaining optimization; counting
the move and ALU pairs that could fuse suggests roughly 40-45 cycles per
operator, an estimate from reading the code rather than a measurement, and
one that would still leave nine channels over budget at 32.780 kHz before the
envelope is added. The synthesis loop alone also occupies 495 of the 512
internal program words `Dsp_ExecBoot` can load, so a complete kernel needs
external program memory or a two-stage loader.

The exact per-sample envelope is what drives this. Because it advances every
sample under a chip-wide counter, the operator-major block rendering that
makes F030MXDRV's YM2151 kernel affordable is unavailable, and every operator
pays its full per-sample cost with no amortization.

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

Start the practical prototype at 32.780 kHz, matching the successful MXDRV
architecture. Phase, envelopes and LFO clocks must be correctly retimed;
merely changing the output divider is wrong. Native-rate OPL synthesis plus
resampling is a separate, tighter budget. Codec-rate synthesis changes
aliasing and feedback behavior and must be assessed against an OPL reference.

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

Keep the existing iMUSE engine and AdLib instrument/voice driver on the
68030. Implement an `OPL::OPL` backend below `writeReg(int,int)` and send
ordered register writes to the DSP. This preserves the existing arrangement,
hooks, jumps, priorities, instrument definitions and synthesized effects.
The current FCM package is Roland-specific and cannot be substituted for
the ADL resources unchanged.

There are four concrete integration tasks:

1. **Build and factory selection.** The Falcon build disables DOSBox, MAME
   and Nuked OPL, and `USE_NFM` is off. Add the DSP backend to `audio/fmopl.cpp`
   and make `audio/adlib.cpp`'s `ENABLE_OPL3` condition recognize it. Enabling
   an existing software core would run that core on the 68030, not the DSP.
2. **Audio-clock callbacks.** The AdLib driver requests 250 Hz callbacks.
   `Audio::RealChip` caps its host timer at 100 Hz and batches callbacks;
   simply inheriting it would not preserve evenly spaced 4 ms events.
   Advance iMUSE/AdLib in a foreground audio-production timeline and tag
   writes with their intended sample positions. Maintain fractional timing
   and bounded lookahead. The interrupt should service prepared data, not
   execute arbitrary game/parser work.
3. **One owner for Falcon audio.** `AtariMixerManager` currently owns uSound,
   DMA buffers and Timer A; MXDRV configures SSI/crossbar and Timer A itself.
   Integrate those responsibilities rather than starting both owners. A
   promising candidate is DSP FM plus one host-mixed speech/effects stream,
   with the DSP sending final audio directly over SSI. Its transfer and CPU
   costs still need measurement; MXDRV's PDX workload is not ScummVM's mixer.
4. **Transport and state.** OPL uses nine-bit register addresses, so MXDRV's
   eight-bit `02 rr dd` format needs an explicit bank bit or new record.
   Preserve key edges and ordered writes; coalesce only proven-safe writes.
   Handle pause, reset, save/load and game sound-setting changes. Preserve
   mixer volume/mute behavior when music bypasses the CPU sample mixer.

An alternative that reads DSP-generated music back to the CPU mixer should
be measured as an alternative: output download and another CPU mix pass
consume some of the gain from offloading synthesis. Direct SSI is the
recommended first architecture to investigate, not an already-working
ScummVM integration.

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
   envelope, feedback and AM/vibrato semantics. The host reference kernel is
   bit exact against Nuked-OPL3, including on the captured stream, and the
   DSP56001 synthesis loop reproduces it word for word at nine and eighteen
   channels while costing 1,025 and 1,983 instruction cycles per frame (see
   above). Still open: the envelope generator, tremolo and vibrato, DSP-side
   register decoding, and transport and SSI in the complete measurement.
   Those additions can only raise a figure that is already over budget, so
   the useful next question is which approximations buy enough back.
3. Compare captured DSP audio and register timing with the reference at the
   same output rate. Check pitch, attack/release, low-level envelopes,
   vibrato/tremolo, feedback spectra and transients, then audition actual
   music and effects. Reduced sample rate alone is not a quality verdict.
4. Integrate the nine-channel Atlantis mode first and measure deadline
   margins while speech, animation, room loads and user interactions run.
   Require no missed or repeated periods, preserved event ordering, and
   measured CPU/DSP worst-period headroom. A proposed engineering target is
   20% worst-period DSP margin; that is a target, not an achieved result.
5. Qualify the paired-channel Sam & Max path, then generic four-operator and
   rhythm modes if desired. Repeat on physical hardware with counters and
   sustained gameplay before declaring the target complete.

**Recommendation, revised 2026-09-16 after the benchmark.** The original
recommendation was to build toward an OPL3-capable DSP renderer and qualify
its OPL2-compatible Atlantis path first. That path is now measured, and an
*exact* renderer does not fit: nine-channel synthesis alone is 209% of the
32.780 kHz budget with the envelope generator still absent, and optimization
of the remaining kind cannot close a gap that size.

What remained open was an *approximate* renderer, along the lines F030MXDRV
already took for the YM2151: block-rate envelopes and LFO, a lower synthesis
rate with the clocks correctly retimed, and perceptual rather than sample
compatibility. That renderer was then built and measured; the next section
records it. None of these findings make FM timbres sound like an MT-32.

## Outcome

The practical kernel, its DSP implementation, the stream transport and the
ScummVM integration are in [tools/foa-opl3](../tools/foa-opl3/README.md),
each with a gate and a committed result file. In short:

| Measurement | Result |
| --- | ---: |
| Practical kernel against the exact one, sustained tones | levels within 0.1 dB, partials within 0.6 dB, pitch within 0.7 cent |
| Envelope contour correlation, Atlantis 60 s | 0.982, mean level error 0.71 dB, onset skew at most 2.8 ms |
| Aliasing: energy above 3 kHz against the exact kernel, Atlantis 60 s | +0.9 dB at 49.17 kHz (+2.7 dB at 32.78 kHz, heard as blurred instruments) |
| DSP cost at 49.17 kHz, Atlantis first 4 s | 201.1 cycles per frame, 62% of budget, word exact |
| DSP cost at 49.17 kHz, nine feedback FM channels with LFO held | 262.1 cycles per frame, 80% of budget, word exact |
| Stream mode through the SSI | Atlantis 20 s: 1,280 periods; worst-case load 10 s: 640 periods; none late, checksums equal |
| The game on the emulated Falcon | 6,166 periods through 90 s of the opening, none late, 98 extension periods (1.5 s of sequencer slip inside resource loads), opening music recorded |
| Day of the Tentacle, Monkey Island 1 and 2 on the same build | 6,137 / 6,179 periods in 90 s, 7,716 in 120 s; none late; 80 / 97 / 60 extension periods; opening music recorded |

Steps 2 through 4 of the sequence above are therefore done in emulation
with the practical kernel; step 5 (Sam & Max's layered path, hardware) is
not. Production and delivery both run from a Timer A interrupt, the main
loop only mixes PCM ahead, and the game's stalls at scene changes (seconds,
as the transport's PCM underrun count shows) no longer touch the music;
extension periods cover the stretches where the interrupt may not run
iMUSE, at the cost of 14.6 ms of sequencer slip each. The remaining
engineering items are the per-operator boundary pass (the second-largest
DSP cost, unoptimized, and the margin now that the rate is 49.17 kHz), a
listening pass that includes the slips, and a
hardware run with the same counters.

## Evidence and reproduction

Fresh reports, raw DSP profiles, game hashes and binary/source hashes are in
`build-falcon030/opl3-investigation/` (ignored generated artifacts).
`provenance.json` identifies the inspected sources and executed binaries.
The capture harness's own gated result, with source, binary and game hashes,
is in [its results.json](../tools/foa-opl3/results.json). No production backend
or game configuration was changed: the capture replaces the OPL factory only
inside a separate headless executable built in its own tree.

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
