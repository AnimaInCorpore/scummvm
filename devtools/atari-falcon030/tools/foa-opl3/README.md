# AdLib on the Falcon DSP: capture, kernels, transport, integration

2026-09-16. The [OPL3 investigation](../../docs/opl3-feasibility.md) in
five steps: the register stream ScummVM's real AdLib driver produces for
Fate of Atlantis, an exact OPL kernel checked against Nuked-OPL3, its
DSP56001 transliteration (which does not fit), a *practical* block-rate
kernel that does, the same kernel streaming through the Falcon codec, and
the ScummVM build that plays the game with it. Each step has its own gate
and its own committed result file.

| Step | Result | Gate |
| --- | --- | --- |
| Register capture | [results.json](results.json) | `capture-gate.py` |
| Exact host kernel, bit exact against Nuked-OPL3 | [kernel-results.json](kernel-results.json) | `kernel-gate.py` |
| Exact DSP synthesis loop: 209% of budget | [bench-results.json](bench-results.json) | `bench-gate.py` |
| Practical kernel against the exact one | [practical-results.json](practical-results.json) | `practical-gate.py` |
| Practical kernel's register semantics, word for word | [practical-unit-results.json](practical-unit-results.json) | `opl-practical-unit-test` |
| Practical DSP kernel at 49.17 kHz: word exact, 62-80% of budget | [rt-bench-results.json](rt-bench-results.json) | `rt-bench-gate.py` |
| Stream mode through the SSI: word exact, no late period, also under the worst-case load | [rt-stream-results.json](rt-stream-results.json), [rt-stream-stress-results.json](rt-stream-stress-results.json) | `rt-stream-gate.py` |
| Atlantis on the emulated Falcon with the DSP build | [game-results.json](game-results.json) | `game-gate.py` |
| Day of the Tentacle on the same build | [game-results-tentacle.json](game-results-tentacle.json) | `game-gate.py --gameid tentacle` |
| The Secret of Monkey Island (Ultimate Talkie) on the same build | [game-results-monkey.json](game-results-monkey.json) | `game-gate.py --gameid monkey` |
| Monkey Island 2 (Ultimate Talkie) on the same build | [game-results-monkey2.json](game-results-monkey2.json) | `game-gate.py --gameid monkey2 --click` |

The first three sections below are the capture and the exact kernel as
originally measured; the practical kernel and everything after it start at
[The practical kernel](#the-practical-kernel).

## How the capture works

`trace-opl.cpp` replaces `audio/fmopl.o` in a **separate** headless executable,
`scummvm-opl-capture`. The game, its resource selection, iMUSE and
`audio/adlib.cpp` are all unchanged, so what is recorded is what that driver
already decided to send, after its own register cache has dropped redundant
writes. The normal Falcon build and the normal objects are untouched; the
build has its own tree so no object from another experiment can be linked by
accident.

The capture is not an emulator. It deliberately does not take its clock from
the mixer: the AdLib driver asks for 250 Hz and `Audio::RealChip` would cap
that at 100 Hz and batch the difference, which is exactly the behavior the
Falcon backend must avoid. The capture installs the timer at the requested
period and timestamps every write on that grid. The null backend is rebuilt
with the existing `FCM_IMUSE_TEST_CLOCK` virtual clock, so game time and
callback time are the same clock and repeated runs are identical.

Each write is tagged with the context it came from: `t` inside a driver
callback, `g` from the engine thread between callbacks.

## What was measured

Two gated runs per window; each pair produced byte-identical traces. Both
windows are the unattended opening with speech muted and no player input;
both start game sounds 150, 21, 22, 29 and 30, the same cue sequence the
MT-32 reference trace records.

| Measurement | 60 s window | 300 s window |
| --- | ---: | ---: |
| Register writes | 12,247 | 52,356 |
| Mean writes/s | 204.05 | 174.51 |
| Driver callbacks | 15,002 | 75,001 |
| Callback periods off the 4,000 µs grid | 0 | 0 |
| Writes from a driver callback | 12,234 | 52,345 |
| Writes from the engine thread | 13 | 11 |
| Callbacks carrying at least one write | 1,748 | 7,372 |
| Writes in the busiest single callback | 129 | 129 |
| Peak writes in any 10 ms | 138 | 138 |
| Peak writes in any 100 ms | 147 | 147 |
| Peak writes in any 1 s | 489 | 489 |
| Longest gap between writes | 984 ms | 984 ms |
| Peak simultaneously keyed channels | 9 | 9 |
| Mean keyed channels, sampled at callbacks | 4.571 | 5.049 |
| Key-on / key-off edges | 992 / 992 | 4,830 / 4,830 |
| Key-ons on an already-keyed channel | 13 | 13 |
| Distinct registers written | 119 | 119 |
| Distinct register/value pairs | 1,581 | 1,622 |
| Writes to the second register bank | 0 | 0 |
| Writes to the rhythm register `0xBD` | 0 | 0 |

The extra 240 seconds add no new cue and no denser passage: every peak window
above is set inside the first 60 seconds. The mean rate falls because the
later material is sparser, not because the burst behavior changed.

### What the stream requires of a backend

- **The traffic is bursty and tick-aligned.** Nearly all writes arrive inside
  a 250 Hz callback, and the busiest callback carries 129 of them while the
  median callback carries none. Sizing a host-port transport on the 204
  writes/s mean would be wrong by a factor of about 160 against that burst.
- **A small number of writes are not tick-aligned.** Thirteen writes in the
  60-second window come from the engine thread between callbacks. They are
  rare but they exist, so ordering cannot be assumed to follow tick boundaries.
- **Nine voices are genuinely used.** Occupancy reaches all nine channels and
  averages about five. Only 13 key-ons in either window land on a channel
  that was still keyed, so voice reuse happens but is not the common case in
  this material. This is measured occupancy for these runs, not a worst case
  for the whole game.
- **The driver forwards raw instrument bytes.** 3,991 of the 6,789 waveform
  select writes in the 300-second window carry bits a YM3812 ignores; raw
  values reach `0x7e`. `adlibSetupChannel()` writes `instr->modWaveformSelect`
  unmasked. A backend must mask to the chip's significant bits, not reject or
  trust the value. All four OPL2 waveforms are used after masking.
- **No feature can be scoped out of the kernel.** Tremolo, vibrato, the
  sustaining envelope flag and key-scale rate are all set by real instruments;
  feedback takes every value from 0 to 7; both connection types appear, though
  additive is rare (5 writes). Four-operator mode, the second register bank
  and rhythm mode are never touched.
- **`0xBD` is never written.** The driver's `adlibWrite(0xBD, 0)` at open is
  absorbed by its own register cache, which starts zeroed. Only two chip
  control writes reach the chip at all: `0x08 = 0x40` and `0x01 = 0x20`, the
  latter enabling waveform select. A backend must therefore come up with
  rhythm mode off and AM/vibrato depth zero by itself.

## The synthesis kernel

2026-09-16, step 2 of the sequence, in progress. `opl-kernel.h` is a
two-operator OPL kernel written in the arithmetic a DSP56001 can execute
directly: flat arrays, index routing instead of pointers, no division, and no
value wider than the 24-bit word the target has. It is the reference the DSP
assembly is transliterated from, and the oracle that assembly will be checked
against.

**It is bit exact against Nuked-OPL3**, which is a chip model rather than an
approximation, so the bar is sample equality and not a tolerance.
[kernel-results.json](kernel-results.json) records the gated run:

| Check | Result |
| --- | ---: |
| Samples compared | 4,189,988 |
| Register writes applied | 47,846 |
| Mismatches | 0 |

The cases are: every frequency multiplier, key-scale rate, block and a spread
of f-numbers; every attack/decay pair and every sustain/release pair in both
envelope types; every key-scale level against total levels and blocks; all
waveforms with both connection types and every feedback depth, in OPL2 and
OPL3; tremolo and vibrato, shallow and deep, over a full LFO period; nine and
eighteen channels with key cycling and mid-note patch reloads; and the
captured 60-second Atlantis register stream replayed at its recorded times.

Two-operator melodic channels are the whole scope. Hardware four-operator
pairing and rhythm mode are absent, because neither ScummVM AdLib driver
enables them and the captured stream never touches them.

### What this settled about the DSP implementation

- **The waveform ROM is 512 words, not 8,192.** A desktop build uses eight
  unpacked 1,024-entry tables; all 8,192 of those entries reproduce exactly
  from a 256-entry quarter log-sine table plus index and sign arithmetic.
  With the 256-entry exponential ROM that is 512 words, which is exactly the
  DSP56001's internal X and Y data RAM. The feasibility note's 24 KiB
  external waveform reservation is not needed.
- **Both ROMs are closed-form**, so [generate-tables.py](generate-tables.py)
  computes them rather than copying them, and the gate checks the result
  against the Nuked ROMs in this tree.
- **External memory is not the constraint it looked like.** F030MXDRV
  measured the Falcon's DSP SRAM at zero wait states on all three external
  paths once the bus control register is cleared, which reset does not do on
  the `Dsp_ExecBoot` path. Placement still matters for Hatari's two-cycle
  penalty when one instruction reaches two external spaces, which argues for
  splitting an external X plus external Y parallel move into two
  instructions rather than combining them.
- **An exact OPL envelope forbids block rendering.** The envelope generator
  advances every sample and its rate machine is driven by a chip-wide
  counter, so the operator-major block loop that makes F030MXDRV's YM2151
  kernel affordable is not available without approximating it. A faithful
  kernel has to be frame-major. This is the main open cost question.

## The DSP kernel and its cost

[dsp/opl.asm](dsp/opl.asm) is the DSP56001 transliteration of that kernel's
synthesis loop, driven by [m68k/oplbench.s](m68k/oplbench.s) on an emulated
Falcon. It renders frames the gate compares word for word against the host
reference, and Hatari's DSP profiler measures the loop between two labels.

**Both configurations render output identical to the host reference**, so the
loop being measured is the real algorithm rather than a stand-in.
[bench-results.json](bench-results.json) records the gated run.

| | 9 channels, 18 operators | 18 channels, 36 operators |
| --- | ---: | ---: |
| Frame words compared | 2,048 | 2,048 |
| Mismatches against the host kernel | 0 | 0 |
| Instructions per frame | 974 | 1,911 |
| **DSP instruction cycles per frame** | **1,025.00** | **1,983.00** |
| Instruction cycles per operator | 56.94 | 55.08 |
| Share of the 24.585 kHz budget | 157% | 304% |
| Share of the 32.780 kHz budget | 209% | 405% |
| Share of the 49.170 kHz budget | 314% | 608% |

The budgets are the same calibrated figures the feasibility note uses:
32,084,988 Hz oscillator, two clocks per instruction cycle, one kernel sample
per output frame.

### What that means

- **Synthesis alone is about twice the budget at nine channels.** The
  original Atlantis AdLib arrangement needs 18 operators, and rendering them
  at 32.780 kHz costs 209% of what the DSP has. Even the lowest sensible
  codec rate, 24.585 kHz, is 157%.
- **And the envelope generator is not in that number at all.** These runs
  hold every envelope constant. Tremolo, vibrato, register decoding on the
  DSP, SSI output and the host transport are all absent too. Every one of
  them adds to the figure; none subtracts.
- **Cost scales with operators, not channels.** 56.94 against 55.08 cycles
  per operator across a 2x change in load, so the 36-operator target is
  simply twice the nine-channel figure. There is no economy of scale to find.
- **Optimization does not look like enough.** The loop uses no parallel X/Y
  moves, which is the obvious remaining win. Counting the move and ALU pairs
  that could fuse suggests roughly 40-45 cycles per operator — an estimate
  from reading the code, not a measurement. Even if it held, nine channels
  would still be over the 32.780 kHz budget before the envelope is added.
- **The program barely fits.** The synthesis loop alone occupies 495 of the
  512 internal program words `Dsp_ExecBoot` can load. Adding the envelope
  generator and a register decoder means external program memory, whose
  instruction fetch then competes with external data access, or the
  two-stage loader F030MXDRV uses.

An exact per-sample OPL envelope is what forces this. It advances every
sample under a chip-wide counter, so the operator-major block rendering that
makes F030MXDRV's YM2151 kernel affordable is unavailable, and every operator
pays its full per-sample cost with no amortization.

### How the arithmetic fits 24 bits

The phase is held shifted left five so its table index is bits 14-23; a right
shift by k is one multiply against a pre-doubled operand by 2^(22-k), which
keeps every table constant inside the signed 24-bit range and replaces a
repeated single-bit shift. The left and right mixes are computed as the plain
operator sum corrected by the delayed group, which is exact because a
two-operator channel sum never overflows sixteen bits, and which costs one
short loop per frame instead of a per-operator branch.

### What the DSP kernel does not implement

The envelope generator, tremolo and vibrato, four-operator mode, rhythm mode,
register decoding, SSI output and the host transport. The nine-channel
fixture exercises mixed connections; the eighteen-channel one uses frequency
modulation throughout, because every OPL3 modulator sits at slot 15 or above
and this kernel only delays carriers.

### What is not established

No hardware run: every cycle figure is Hatari's model, which charges Falcon
external memory zero wait states and which Hatari's own documentation calls
instruction-wise correct rather than cycle accurate. No audio was auditioned,
and no output-rate conversion is modelled.

## The practical kernel

`opl-practical.h` is the renderer the budget allows. It keeps the chip's
1,024-step waveforms, its log-domain envelope in the same 0.1875 dB units,
its feedback and modulation depth and its f-number pitch, and gives up
sample equality in three places:

- **Codec-rate synthesis.** It runs at the Falcon codec's 49,169.92 Hz,
  1.1% below the chip's 49,716 Hz, with the phase, envelope and LFO clocks
  retimed (`generate-tables.py` measures every envelope rate on the exact
  machine and folds it into one step per block). It first ran at the
  codec's 32,780 Hz, which the budget made comfortable; but the bright
  patches (full-level modulators with feedback 4 to 7) put enough energy
  above 16.4 kHz that it folded back as a haze the first listener called
  blurry. Against the exact kernel the Atlantis stream was 2.7 dB louder
  above 3 kHz on average and 8 to 15 dB in the worst stretches; the same
  kernel rendered at twice the rate lost the excess, which made it
  aliasing rather than arithmetic. At 49.17 kHz the fold sits where the
  chip's own does: the excess is 0.9 dB and the mean third-octave
  difference 1.8 dB, from 2.9.
- **Block-rate control.** Envelopes, tremolo and vibrato advance once per
  64-frame block (1.30 ms), which is what lets each operator run as one
  hardware loop over the block, operator-major, the way the sibling YM2151
  kernel does. Decay and release are a per-block step; attack is a
  per-block retention factor fitted to the audible part of the chip's curve
  (full attenuation down to -6 dB) and ended four units above zero, because
  the chip's attack is an exponential with a slow linear tail and a factor
  fitted to its total time started a slow attack 10 ms early. Sustain
  level, key scaling, the envelope-type flag, the silence snap and the
  instant attack keep their chip semantics.
- **Block-boundary writes.** A register write takes effect at the start of
  the block it falls in, so up to 1.30 ms early. Key-on edges are counted,
  not sampled, so an off-then-on inside one block still retriggers.

Register decoding stays on the 68030 in the same header (`Decoder`): each
write becomes a few parameter words for the DSP's operator and channel
records, emitted only when they change, so the DSP's per-block work is
free of register logic and the host pays about a microsecond per write.

What is not given up is the chip's register semantics, and a review on
2026-09-17 found four places where the first version had let them go:

- **Vibrato is the chip's integer arithmetic.** The chip displaces the
  f-number by its top three bits, halved at the odd LFO positions and again
  at the shallow depth, truncating each time, *before* the block and the
  multiplier apply. Scaling one increment by fractional LFO coefficients
  instead gave f-number 200 in block 5 an 8.65 cent vibrato where the chip
  has none at all (f-numbers below 256 have no shallow half-step, below 128
  no vibrato), and 873,349 keyed operator-blocks of the 300 s Atlantis
  trace sit in that range. The decoder now computes the increment at each
  of the five distinct displacements and sends them as offsets; the DSP
  picks one by LFO position, which is cheaper than the multiply it
  replaces. The depth bit is the decoder's business and no longer a DSP
  scalar.
- **A high increment aliases as the chip's does.** The chip's phase is 19
  bits, so a high block under a high multiplier wraps: f-number `0x241`,
  block 7, multiplier 15 plays 2,810 Hz. The increment used to saturate at
  the 24-bit word's limit, which put every such pitch on one 48 Hz tone.
  It is now reduced modulo the chip's phase range, taken as the negative
  frequency it aliases to above half of it, and retimed; the DSP's phase
  arithmetic is modular and takes a negative increment as it is.
- **Decay ends on the chip's equality test.** The chip leaves decay when
  the envelope's top five bits *equal* the sustain level. A level lowered
  under a running decay is never met again and the decay runs on to
  silence; the first version clamped the envelope back to the new level, a
  5 dB jump upwards. Below the level the step may reach it, within one
  sustain step of it the envelope holds where it is, past it the decay
  continues.
- **No attenuation is exact.** A 24-bit fraction cannot hold 1.0, and the
  gain 0.999... truncated every positive product one short: at full level
  the modulator indexed the carrier one table step low through its
  positive half, which moved four samples in ten of a bright patch. The
  gain table now holds half the gain against waveform samples of twice the
  scale, so no attenuation is 0.5 exactly, at no cost on the DSP; the emit
  rounds its master gain product for the same reason (`mpyr` for `mpy`),
  so full volume passes the mix through unchanged. With its increments set
  to the chip's, the kernel then reproduces 97% of the exact kernel's
  samples of a held full-level FM patch within one LSB (59% before),
  falling off only as the control's rounded increment drifts.
- **A reset resets the machine.** `Decoder::reset` sends the whole reset
  state whatever its shadow held (every host-owned word, and the envelope,
  state and applied key-on count the DSP owns), 344 events for nine
  channels, because a cleared shadow alone leaves the DSP playing and then
  swallows the zeros a driver writes to silence it.

`opl-practical-unit-test` holds these to the word against the exact
kernel: all 2,097,152 combinations of f-number, block, multiplier, depth
and LFO position give the exact kernel's increment, aliased and retimed;
576 sustain level writes at three decay rates never lower the attenuation,
and the 552 of them where both envelopes are on the same side of the new
level at the write (the block-rate envelope runs up to a block behind)
settle where the chip's does; a reset under nine held voices leaves the
machine word for word a fresh one, silent from its first block, and a song
started on it renders as on a fresh pair.

[practical-gate.py](practical-gate.py) scores it against the exact kernel
on synthetic scenarios and the captured 60-second Atlantis stream, at each
signal's own rate, on what a listener would notice. Every note of a
single-voice scenario gets its own spectral check, from the note list the
renderer writes; the first version took one spectrum per uninterrupted loud
stretch, and since release tails kept the stretch alive, the feedback and
waveform sweeps were each checked on their first setting only. The note's
frequency separates its harmonics, compared in level one by one, from
everything else: an alias folds around each signal's own Nyquist
frequency, and a phase truncation spur sits where the increment's
fraction puts it, so neither can coincide between two sample rates. They
are held to a level instead (the strongest below -30 dB, or no more than
6 dB above the chip's own strongest). From
[practical-results.json](practical-results.json):

| Scenario | Notes checked | Envelope corr. | Level error mean / max | Partials mean / max | Bands | Pitch | Notes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| sustained tone | 1 | 1.0000 | 0.00 / 0.05 dB | 0.23 / 0.65 dB | 0.13 dB | 0.6 c | |
| pitch sweep, blocks 2-6 | 9 | 0.9998 | 0.01 / 0.30 dB | 0.61 / 1.37 dB | 0.73 dB | 3.2 c | the 3.2 c is 0.24 Hz at 129 Hz |
| envelopes, both types | 10 | 0.9999 | 0.09 / 1.35 dB | 1.42 / 4.43 dB | 1.35 dB | 0.7 c | partial error is inside slow attacks |
| four waveforms, both connections | 8 | 0.9996 | 0.09 / 0.99 dB | 1.53 / 3.79 dB | 0.76 dB | 0.7 c | |
| feedback 0-7 at four modulator levels, held | 32 | 1.0000 | 0.02 / 0.47 dB | 1.14 / 5.81 dB | 1.67 dB | 3.2 c | 7 notes graded on bands alone, see below |
| every multiplier at block 7, held | 16 | 1.0000 | 0.00 / 0.14 dB | 0.55 / 1.10 dB | 0.02 dB | 0.2 c | to 24.5 kHz; eight are past half the chip's phase range and play its alias |
| tremolo, both depths | 2 | 1.0000 | 0.01 / 0.16 dB | | | | depth 1.72 vs 1.71 dB, 5.33 vs 5.22 dB |
| vibrato, both depths, and none | 3 | 1.0000 | 0.00 / 0.12 dB | | | | depth 11.5 vs 11.8 c, 26.4 vs 26.5 c (13.7 and 27.3 before), period 165 ms both; f-number 200 stays unmodulated in both |
| nine-channel polyphony | | 0.9946 | 0.07 / 0.88 dB | | | | |
| Atlantis, 60 s | | 0.9821 | 0.70 / 7.89 dB | | | | mix of coincident partials; +0.6 dB in 8-15 kHz |

The onset skew is at most 2.7 ms on the synthetic scenarios and 2.8 ms on
the trace (10.4 ms before the attack factor was refitted). The polyphonic
peak errors come from partials of different channels adding with phases
the block quantization shifts, not from levels. The envelope figures let
each 20 ms window match anywhere within the range of its neighbours, so
an onset straddling a window edge is no error; they grade the loudness
contour and say nothing about spectra. The same windows one to one differ
by 0.04 to 0.32 dB on the synthetic scenarios and 1.09 dB on the trace
(`envelope_db_mean_abs_unaligned`).

**Strong feedback is not periodic, on the chip either.** Isolated, a
full-level modulator at feedback 5, 6 and 7 differs from the exact kernel
by 6, 13 and 21 dB per partial on average, with the RMS level equal, and
the review asked whether that is arithmetic. It is not: the exact kernel
differs from *itself* by 2, 10 and 19 dB between the window and two later
windows of the same held note. The oscillator has left the periodic
regime, its partials wander, and a reference that fails the partial test
against itself cannot serve for it. The gate therefore measures the exact
kernel against itself wherever a scenario holds its notes at a constant
level, and grades such a window (7 of the 32) on what the chip does
reproduce: third-octave bands, within 1.67 dB, and the inharmonic peaks.
Two arithmetic candidates were tried on the way and neither is the
cause: summing the two history products before the shift, as the chip
does, instead of truncating each (0.30 to 0.18 dB in the periodic regime,
nothing past it), and the exact unity gain above, which moves samples and
none of these figures. What remains in the periodic regime is the rate:
the same patch at another sample rate has other aliases and other
truncation spurs.

The gate was also run on the kernel as committed before the review's
fixes: it fails the vibrato scenario (11.5 c of vibrato on the note the
chip plays with 1.7 c of measurement jitter) and all of the high-pitch one.

## The practical DSP kernel and its cost

[dsp/oplrt.asm](dsp/oplrt.asm) implements that kernel as a memory-image
machine: the host writes operator and channel records, the DSP runs the
per-block boundary pass and the per-frame stages. The stages are the
sibling kernel's shapes with OPL semantics: the phase in accumulator B with
the mask constant doubling as the increment multiplicand, the waveform held
as linear samples so the envelope is one multiply, feedback history in
internal X and Y with an alternating gain pair, four to twelve instructions
per frame per operator. Silent channels are skipped and idle operators
short-circuit the boundary pass.

[rt-bench-gate.py](rt-bench-gate.py) renders on the emulated Falcon,
compares every output word with the host reference and attributes Hatari's
DSP profile by code range. From [rt-bench-results.json](rt-bench-results.json):

| | Nine feedback FM channels, tremolo and vibrato, held | Atlantis, first 4 s |
| --- | ---: | ---: |
| Frames compared | 49,152 | 196,672 |
| Mismatches | 0 | 0 |
| Stages (per-frame) | 183.8 | 130.6 |
| Per-operator boundary pass | 48.8 | 42.8 |
| Loaders, modes, driver | 11.9 | 10.9 |
| Block and channel boundary | 11.0 | 10.4 |
| Emit, clear, events | 7.1 | 7.0 |
| **Instruction cycles per frame** | **262.7** | **201.6** |
| Share of the 326.3-cycle budget at 49.17 kHz | 81% | 62% |

A third case, `paths`, is there for exactness alone: the stress case plus
what music rarely does, so that the DSP's code for it is compared word for
word too. A sustain level lowered under a running decay (127 blocks past
the level, and the block that enters sustain above it), increments past
half the chip's phase range with a deep vibrato straddling it (109 blocks
with a negative increment), and a chip reset under held notes followed by
a song whose zero writes a stale shadow would swallow: 49,152 frames, no
mismatch. The gate fails if the case stops reaching any of these paths.

Everything but the stages is per-block work, and that is why the block is
64 frames. At 48 frames (the 0.98 ms of the first version's 32 frames at
32.78 kHz) the same two cases cost 88% and 69%, which bit-exactness and the
budget arithmetic accepted and the transport did not: the kernel idles for
a millisecond or so per period while the host's 1 kHz tick notices READY
and sends the payload, so the stress case overran in the stream gate, and
so did Atlantis's densest passage in the game. The boundary pass still
walks the record with indexed accesses and remains the obvious next
optimization. The whole program is 1,249 words; the stages and the
operator pass live in the 512 words of internal program RAM, everything
else in external.

## Stream mode

The same program owns the codec in production: a 3,072-word SSI ring holds
two 768-frame periods (15.62 ms each) of interleaved stereo, transmitted
under interrupt through r6, and each period arrives from the host as events
plus 192 mono PCM samples at a quarter of the codec rate (12,292 Hz),
expanded by linear interpolation and added to the FM mix under a master
gain. The DSP acknowledges a payload before rendering it, with its period
and late counters in the acknowledgement, so the host stays one period
ahead; a period that is not ready in time repeats and is counted. The
first period waits until the transmitter has just entered the ring half it
is about to render, so it gets a whole half like every later one and the
stream does not open with a late period.

[m68k/oplplay.s](m68k/oplplay.s) drives that protocol with direct host-port
writes, and [rt-stream-gate.py](rt-stream-gate.py) checks the emitted words
by checksum against the host reference. From
[rt-stream-results.json](rt-stream-results.json): 20 s of the Atlantis
stream, 1,280 periods submitted and rendered, none late, checksum equal;
and from [rt-stream-stress-results.json](rt-stream-stress-results.json),
`--scenario stress`: 10 s of nine feedback FM channels with both LFOs held,
640 periods, none late, checksum equal.

## In the game

The ScummVM build wires it in without touching the AdLib driver:

- `backends/platform/atari/atari-dsp.cpp` boots the kernel through the
  two-stage loader (`dsp-opl-image.h`, generated by `build-dsp.sh`),
  uploads the tables, routes the DSP's SSI to the DAC at 49.170 kHz and
  runs both delivery and production from an MFP Timer A interrupt at about
  1 kHz. Delivery takes one protocol step per tick: announce, READY, a
  paced blast, acknowledgement. Production runs whenever fewer than four
  periods are queued and the interrupted code holds no mutex and is outside
  the allocator (`atari-critical.h`: the Atari's mutexes count instead of
  locking, and so does the allocator), at the program's interrupt level on
  its own stack with the FPU state saved; it runs the OPL's timer
  callbacks, which are the AdLib driver and iMUSE's sequencing. When the
  main loop holds a critical section for longer than the queue lasts (a
  resource load holds SCUMM's resource mutex for over 100 ms) or a
  production call runs long (iMUSE spends up to a quarter second in one
  callback at a jump), the tick submits extension periods without
  callbacks: the voices carry on and the sequencer slips 15.6 ms each,
  where the kernel would otherwise loop its last period.
- `backends/platform/atari/dsp-opl.cpp` is the `OPL::OPL` backend
  (`opl_driver=atari_dsp`, OPL2 only): register writes go through the
  decoder into the period being produced, and the driver's 250 Hz callbacks
  run inside period production on the audio clock, so each callback's
  writes land in the block that corresponds to its time. The kernel
  outlives every OPL, so creating one, resetting it and destroying it each
  send the decoder's full reset; writes made between periods wait in a
  queue that outlives the instance, because the last of them (the reset a
  closing driver leaves behind) has no instance left to deliver it. When
  the DSP stream is off or the DSP was not available, the driver entry
  falls back to the first other OPL2 emulator instead of returning nothing
  to a caller that does not expect that.
- `AtariMixerManager` in DSP mode mixes speech and effects at 12,292 Hz
  mono on the main loop, sixteen period chunks ahead (250 ms) into a ring the
  interrupt takes from (the mixer's read path streams speech from disk,
  which only the main loop can do), and forwards the plain sound type's
  volume and mute as the kernel's master gain: what the mixer gives a
  software OPL's stream. The engine has already put the music slider into
  the operator levels it writes (iMUSE's `setMusicVolume`), so the music
  type's volume on top, as first committed, applied it twice, 6 dB too
  quiet at half volume. A loop stall longer than the ring silences speech
  and effects until the loop runs again; the music does not notice.
  `atari_dsp_audio=false` restores DMA playback.

[game-gate.py](game-gate.py) runs Atlantis on the emulated Falcon with that
build, records Hatari's DAC output and reads the transport's counters from
the log. From [game-results.json](game-results.json): the kernel boots, the
game starts, 6,175 periods stream through 90 s of its opening with no
protocol error and no late period, and no tick found the queue empty once
the stream was running. The loop stalled for 2,538 periods of PCM (39.6 s, nearly all
of it engine start-up before the first scene, the rest scene changes) while
the music went on, and 96 extension periods (1.4 s of sequencer slip)
covered resource loads of up to 121 ms and one iMUSE callback of 245 ms.
The recording carries the opening music at -12 dBFS peak.

The build's profile admits three more DOS games on the same AdLib driver,
and each runs the same gate on the same binary with the same shape of
result: no late period; no tick with an empty queue once the stream runs;
no protocol error; its opening music in the recording. Speech is off in all
of them (the gate mutes it, and these editions ship theirs as FLAC, which
this build does not decode). Monkey Island 2 opens on a difficulty screen
and waits for a click, so its run is 120 s with `--click 45:24:64`; the
gate parks the cursor in a corner and moves it, since Hatari only injects
relative motion, and the screenshot it takes at the end shows where the
game got to.

| Game | Periods | Extension periods | Music peak | Results |
| --- | ---: | ---: | ---: | --- |
| Day of the Tentacle, CD | 6,160 in 90 s | 69 | -24 dBFS | [game-results-tentacle.json](game-results-tentacle.json) |
| The Secret of Monkey Island, Ultimate Talkie | 6,203 in 90 s | 102 | -24 dBFS | [game-results-monkey.json](game-results-monkey.json) |
| Monkey Island 2, Ultimate Talkie | 7,718 in 120 s | 53 | -17 dBFS | [game-results-monkey2.json](game-results-monkey2.json) |

So a v6 game and the two Monkey Islands fit in the 14 MB beside the DSP
transport at least through their openings.

Two earlier stages of the Atlantis run are worth keeping in mind. With
production on the main loop and delivery from the interrupt it reported two
late periods; but the kernel counts one late per starvation, not per
period, so each of those was the music frozen for the length of a scene
change, which the PCM underrun count now makes visible. With everything on
the main loop it had 171 late periods, 3.7%. The emulator's DSP timing is a
model; nothing here ran on hardware, and the recording was checked for
presence and level, not auditioned.

## What this does not establish

- Nothing ran on hardware: every cycle figure and every deadline is Hatari's
  model, which charges Falcon external memory zero wait states and calls its
  DSP emulation instruction-wise correct rather than cycle accurate.
- No listening test. The practical kernel's quality is the perceptual gate's
  numbers; the game recording was checked for presence and level, not heard.
- One unattended opening sequence per game, with no player input beyond
  Monkey Island 2's one click and no other scenes.
  Dense gameplay, menus, save/load and MIDI-driven effects outside it are
  not covered, and the 68030's load in the game is not measured: the
  transport's counters (late, PCM underruns, extension periods, the longest
  refusal streak and production call) are where a starved kernel or a
  stalled loop shows, and the opening's stalls are all covered by the
  extension periods.
- One Monkey Island 2 run died in an address error inside TOS before any
  input, with an `rts` popping a corrupt return address, on the handler as
  first committed. The handler now moves to its own stack before anything
  else, so the interrupted stack, which may be TOS's small one inside a
  BIOS or XBIOS call, carries only the exception frame and the saved
  registers; the fault has not recurred in the nine runs since, across all
  four games, but its cause was not established.
- The sequencer's slips are not audited: an extension period delays iMUSE
  by 15.6 ms, 1.5 s over the opening, inside resource loads and a long
  callback, and nobody has listened for them. Running iMUSE in interrupt
  context rests on the same guarantee the threaded backends rely on, that
  everything it shares with the engine sits behind a mutex; the Atari's
  mutexes count so the interrupt can stay out, but nothing verifies that
  the engine locks everything it should.
- Sam & Max data was not available here, so the layered OPL3 path remains
  unmeasured; the kernel and decoder support eighteen channels, the DSP
  image is built for nine.
- The exact kernel's sample equality holds at the chip's native 49,716 Hz;
  the practical kernel is not sample exact by design.
- The game gate runs at full music volume, where both volume paths give
  the same level; that the slider is now applied once was read from the
  code (`audio/chip.cpp` plays the software OPL on the plain sound type),
  not measured in a game. The reset path is covered by the unit test and
  the `paths` bench, not by a game that restarts its music driver.
- Aliases and phase truncation spurs are where the codec rate puts them,
  not where the chip's 49,716 Hz does (for f-number `0x1e5` in block 3 the
  chip's spurs fall within 1 Hz of its harmonics, the kernel's 27 Hz above
  them, at about the same level). Only synthesis at the chip's rate and a
  resampler would change that. Feedback 5 to 7 under a loud modulator is
  compared by bands, because neither kernel reproduces its own partials
  there.

## Reproducing

```sh
sh devtools/atari-falcon030/tools/foa-opl3/build-capture.sh
python3 devtools/atari-falcon030/tools/foa-opl3/capture-gate.py \
  --game /path/to/atlantis-cd --output build-falcon030/opl3-capture
python3 devtools/atari-falcon030/tools/foa-opl3/test-analyze-opl.py
```

The gate runs the capture twice, refuses to report anything if the two traces
differ, and writes `results.json` alongside the retained runs. [results.json](results.json)
is the committed 60-second result, including source, binary and game hashes.
`--milliseconds` selects the window. Traces, binaries and game data stay
outside tracked files or inside the ignored `build/` directory.

`analyze-opl.py` can also be run on a single trace, and has unit tests
covering register classification, the sliding windows, trace validation and
the key-edge accounting.

The DSP benchmark needs the sibling project's toolchain: `asm56000` under
dosbox-staging, `vasm`/`vlink`, and the DSP-calibrated Hatari.

```sh
sh devtools/atari-falcon030/tools/foa-opl3/build-dsp.sh
python3 devtools/atari-falcon030/tools/foa-opl3/bench-gate.py \
  --output build-falcon030/opl3-bench
```

The kernel gate needs a configured host build and a captured trace:

```sh
make -C devtools/atari-falcon030/tools/foa-opl3/build/headless \
  -f Makefile -f ../../kernel.mk opl-kernel-test
python3 devtools/atari-falcon030/tools/foa-opl3/kernel-gate.py \
  --trace build-falcon030/opl3-capture/run-a/opl-writes.ev \
  --output build-falcon030/opl3-kernel
```

The practical kernel's gates, in the same tree:

```sh
make -C devtools/atari-falcon030/tools/foa-opl3/build/headless \
  -f Makefile -f ../../kernel.mk opl-practical-unit-test opl-practical-test opl-rt-fixture
devtools/atari-falcon030/tools/foa-opl3/build/headless/opl-practical-unit-test
python3 devtools/atari-falcon030/tools/foa-opl3/practical-gate.py \
  --trace <opl-writes.ev> --seconds 60 --wav --output build-falcon030/opl3-practical
sh devtools/atari-falcon030/tools/foa-opl3/build-dsp.sh
python3 devtools/atari-falcon030/tools/foa-opl3/rt-bench-gate.py \
  --trace <opl-writes.ev> --seconds 4 --output build-falcon030/opl3-rt-bench
python3 devtools/atari-falcon030/tools/foa-opl3/rt-stream-gate.py \
  --trace <opl-writes.ev> --seconds 20 --output build-falcon030/opl3-rt-stream
```

`build-dsp.sh` also regenerates `backends/platform/atari/dsp-opl-image.h`,
which the Falcon build embeds; after a kernel change rebuild ScummVM with
`backends/platform/atari/build-falcon030.sh` and run the game gate:

```sh
python3 devtools/atari-falcon030/tools/foa-opl3/game-gate.py \
  --game /path/to/atlantis-cd --seconds 60 --output build-falcon030/opl3-game
```

The game needs Falcon TOS 4.04 under Hatari (TOS 4.02, which the kernel
benches boot, dies in the game's video mode switch); `--fpu 68882` is
required by the build. `--wav` on the practical gate keeps WAV files of
both kernels for auditioning, and the game gate keeps Hatari's recording.
