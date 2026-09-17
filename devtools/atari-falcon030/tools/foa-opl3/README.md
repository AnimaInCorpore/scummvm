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
| Practical DSP kernel: word exact, 55-69% of budget | [rt-bench-results.json](rt-bench-results.json) | `rt-bench-gate.py` |
| Stream mode through the SSI: word exact, no late period | [rt-stream-results.json](rt-stream-results.json) | `rt-stream-gate.py` |
| Atlantis on the emulated Falcon with the DSP build | [game-results.json](game-results.json) | `game-gate.py` |
| Day of the Tentacle on the same build | [game-results-tentacle.json](game-results-tentacle.json) | `game-gate.py --gameid tentacle` |

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

- **Codec-rate synthesis.** It runs at the Falcon codec's 32,779.9479 Hz
  instead of the chip's 49,716 Hz, with the phase, envelope and LFO clocks
  retimed (`generate-tables.py` measures every envelope rate on the exact
  machine and folds it into one step per block). Partials above 16.4 kHz
  alias; in the brightest Atlantis passage the 8-15 kHz band carries 3.4 dB
  more energy than the exact kernel's.
- **Block-rate control.** Envelopes, tremolo and vibrato advance once per
  32-frame block (0.98 ms), which is what lets each operator run as one
  hardware loop over the block, operator-major, the way the sibling YM2151
  kernel does. Attack is a per-block retention factor, decay and release a
  per-block step; sustain level, key scaling, the envelope-type flag, the
  silence snap and the instant attack keep their chip semantics.
- **Block-boundary writes.** A register write takes effect at the start of
  the block it falls in, so up to 0.98 ms early. Key-on edges are counted,
  not sampled, so an off-then-on inside one block still retriggers.

Register decoding stays on the 68030 in the same header (`Decoder`): each
write becomes a few parameter words for the DSP's operator and channel
records, emitted only when they change, so the DSP's per-block work is
free of register logic and the host pays about a microsecond per write.

[practical-gate.py](practical-gate.py) scores it against the exact kernel
on synthetic scenarios and the captured 60-second Atlantis stream, at each
signal's own rate, on what a listener would notice. From
[practical-results.json](practical-results.json):

| Scenario | Envelope corr. | Level error mean / max | Partials mean / max | Pitch | Notes |
| --- | ---: | ---: | ---: | ---: | --- |
| sustained tone | 1.0000 | 0.00 / 0.03 dB | 0.29 / 0.56 dB | 0.4 c | |
| pitch sweep, blocks 2-6 | 0.9975 | 0.04 / 0.50 dB | 0.32 / 0.77 dB | 3.4 c at 73 Hz (0.14 Hz) | |
| envelopes, both types | 0.9999 | 0.10 / 1.88 dB | 1.51 / 4.56 dB | 0.4 c | partial error is inside slow attacks |
| feedback 0-7 | 0.9970 | 0.06 / 1.52 dB | 0.42 / 1.31 dB | 0.5 c | |
| four waveforms, both connections | 0.9995 | 0.08 / 1.14 dB | 0.39 / 0.76 dB | 0.2 c | |
| tremolo, both depths | 0.9998 | 0.01 / 0.69 dB | | | depth 1.72 vs 1.70 dB, 5.33 vs 5.21 dB |
| vibrato, both depths | 0.9998 | 0.01 / 0.56 dB | | | depth 11.5 vs 13.5 c, 26.4 vs 27.1 c, period 165 ms both |
| nine-channel polyphony | 0.9947 | 0.07 / 0.81 dB | | | |
| Atlantis, 60 s | 0.9733 | 0.86 / 9.78 dB | | | mix of coincident partials; +3.4 dB in 8-15 kHz |

The onset skew is at most 1.6 ms on the synthetic scenarios and 7.5 ms on
the trace, where the exact chip starts a slow attack on its next rate tick.
The polyphonic peak errors come from partials of different channels adding
with phases the block quantization shifts, not from levels.

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
| Frames compared | 32,768 | 131,104 |
| Mismatches | 0 | 0 |
| Stages (per-frame) | 187.6 | 133.6 |
| Per-operator boundary pass | 97.0 | 84.6 |
| Loaders, modes, driver | 23.8 | 21.9 |
| Block and channel boundary | 22.2 | 20.8 |
| Emit, clear, events | 7.0 | 7.1 |
| **Instruction cycles per frame** | **338.2** | **268.7** |
| Share of the 489.4-cycle budget | 69% | 55% |

The boundary pass is the obvious next optimization (it walks the record
with indexed accesses), but the budget no longer needs it. The whole
program is 1,221 words; the stages and the operator pass live in the 512
words of internal program RAM, everything else in external.

## Stream mode

The same program owns the codec in production: a 1,920-word SSI ring holds
two 480-frame periods of interleaved stereo, transmitted under interrupt
through r6, and each period arrives from the host as events plus 160 mono
PCM samples at a third of the codec rate, expanded by linear interpolation
and added to the FM mix under a master gain. The DSP acknowledges a payload
before rendering it, with its period and late counters in the
acknowledgement, so the host stays one period ahead; a period that is not
ready in time repeats and is counted.

[m68k/oplplay.s](m68k/oplplay.s) drives that protocol with direct host-port
writes, and [rt-stream-gate.py](rt-stream-gate.py) checks the emitted words
by checksum against the host reference. From
[rt-stream-results.json](rt-stream-results.json): 20 s of the Atlantis
stream, 1,365 periods submitted and rendered, none late, checksum equal.

## In the game

The ScummVM build wires it in without touching the AdLib driver:

- `backends/platform/atari/atari-dsp.cpp` boots the kernel through the
  two-stage loader (`dsp-opl-image.h`, generated by `build-dsp.sh`),
  uploads the tables, routes the DSP's SSI to the DAC at 32.780 kHz and
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
  callbacks: the voices carry on and the sequencer slips 14.6 ms each,
  where the kernel would otherwise loop its last period.
- `backends/platform/atari/dsp-opl.cpp` is the `OPL::OPL` backend
  (`opl_driver=atari_dsp`, OPL2 only): register writes go through the
  decoder into the period being produced, and the driver's 250 Hz callbacks
  run inside period production on the audio clock, so each callback's
  writes land in the block that corresponds to its time.
- `AtariMixerManager` in DSP mode mixes speech and effects at 10,927 Hz
  mono on the main loop, eight period chunks ahead into a ring the
  interrupt takes from (the mixer's read path streams speech from disk,
  which only the main loop can do), and forwards the music volume as the
  kernel's master gain. A loop stall longer than the ring silences speech
  and effects until the loop runs again; the music does not notice.
  `atari_dsp_audio=false` restores DMA playback.

[game-gate.py](game-gate.py) runs Atlantis on the emulated Falcon with that
build, records Hatari's DAC output and reads the transport's counters from
the log. From [game-results.json](game-results.json): the kernel boots, the
game starts, 6,334 periods stream through 90 s of its opening with no
protocol error and one late period, the first (the kernel starts
transmitting before the host has a period for it); after that no tick found
the queue empty. The loop stalled for 2,875 periods of PCM (42 s, nearly all
of it engine start-up before the first scene, the rest scene changes) while
the music went on, and 102 extension periods (1.5 s of sequencer slip)
covered resource loads of up to 136 ms and one iMUSE callback of 253 ms.
The recording carries the opening music at -12 dBFS peak.

The build's profile also admits Monkey Island 2 and Day of the Tentacle,
the other two DOS games on the same AdLib driver. Day of the Tentacle (CD
data, speech off, since its speech comes FLAC-compressed and this build has
no decoder) runs the same 90 s gate with the same shape of result, in
[game-results-tentacle.json](game-results-tentacle.json): 6,298 periods,
one late (the first), no empty tick after it, 85 extension periods, and its
intro music in the recording at -24 dBFS peak; so a v6 game fits in the
14 MB beside the DSP transport at least through its intro. Monkey Island 2
has not been run, for want of data.

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
- One unattended opening sequence per game, with no player input and no
  other scenes; Monkey Island 2 not at all.
  Dense gameplay, menus, save/load and MIDI-driven effects outside it are
  not covered, and the 68030's load in the game is not measured: the
  transport's counters (late, PCM underruns, extension periods, the longest
  refusal streak and production call) are where a starved kernel or a
  stalled loop shows, and the opening's stalls are all covered by the
  extension periods.
- The sequencer's slips are not audited: an extension period delays iMUSE
  by 14.6 ms, 1.5 s over the opening, inside resource loads and a long
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
  -f Makefile -f ../../kernel.mk opl-practical-test opl-rt-fixture
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
