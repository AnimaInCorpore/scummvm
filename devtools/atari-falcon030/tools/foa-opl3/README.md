# Atlantis OPL register capture

2026-09-16. First step of the [OPL3 investigation's implementation
sequence](../../docs/opl3-feasibility.md): record the register writes that
ScummVM's real AdLib driver makes while the game runs, so a DSP renderer can
be designed against a measured stream instead of resource counts.

**This measures register traffic and keyed-voice occupancy. It renders no
audio, reaches no OPL emulator, and times nothing on a Falcon or a DSP.**
The capture is the input a renderer would have to consume; it says nothing
about whether a renderer can keep up.

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

## What this does not establish

- No audio was rendered and nothing was compared against an OPL reference.
  Step 3 of the sequence still owns that.
- No DSP kernel exists, so no synthesis, transport or deadline cost was
  measured, on hardware or in emulation.
- One unattended opening sequence, with no player input and no other scenes.
  Scene changes and overlapping cues are present; dense gameplay, menus,
  save/load and MIDI-driven effects outside this sequence are not.
- Sam & Max data was not available here, so the layered OPL3 path the plan
  asks for separately remains unmeasured.
- Register counts are not instruction counts. Nothing here predicts what a
  68030 or a DSP56001 would spend servicing this stream.
- The kernel's sample equality holds at the chip's native 49,716 Hz. No
  conversion to a Falcon codec rate is modelled, and no audio was auditioned.

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
