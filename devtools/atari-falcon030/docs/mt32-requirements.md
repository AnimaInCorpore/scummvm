# Faithful MT-32 for Fate of Atlantis on a stock Falcon: the requirements

What a faithful on-machine MT-32 has to do for *this game*, what the game
actually asks for, and what the measured Falcon budget supplies.

Supply figures are cited from the F030MT32 project
(`/Users/saschaspringer/Work/F030MT32/docs/`), which measured them. Demand
figures are measured here from the game's own data by
[`tools/foa-mt32-demand.py`](../tools/foa-mt32-demand.py). Anything neither
measured nor cited is marked unverified.

## The short answer

**Measured, not projected: this game costs 2.24 LA partials per note, and the
best measured Falcon configuration covers 50.1% of its playing time.** The
other half steals voices, which is by definition not the faithful target. Mean
demand while music sounds is 12.70 of the MT-32's 32 partials; the Falcon
supplies 3-5 exact or 5-8 approximate.

The PCM ROM remains an absolute obstacle rather than a budget one, but this
game leans on the rhythm part far less than expected - 6.5% of playing time -
which changes where that obstacle sits on the critical path.

## What "faithful" requires

From [`mt32-ground-truth.md`](/Users/saschaspringer/Work/F030MT32/docs/mt32-ground-truth.md),
which reads these out of the vendored Munt source:

| Requirement | Value |
| --- | --- |
| Sample rate | 32,000 Hz |
| Partial pool | 32 |
| Parts | 9 (eight melodic plus rhythm) |
| PCM samples in ROM | 262,144, 16-bit logarithmic, bit-permuted on load |
| Control ROM | 64 KiB (v1.0x) - a **runtime** dependency for its data tables, not just firmware |
| PCM ROM | 512 KiB (MT-32) or 1 MiB (CM-32L); **MT-32 suffices here**, see below |
| Reverb | 3 allpasses + 4 combs per mode, ~11,300 samples of delay line |

Two structural points from that document matter for feasibility. In favour:
the LA32 works in log space, so partials carry no filter state, are
independent, and are dominated by table lookups and adds - about 1,500 DSP
words of tables. Against: the control ROM's tables (PCM wave table, timbre
maps for groups A and B, rhythm map, reserve/pan/program defaults, parameter
maxima) are needed at runtime even though the 8095 is not emulated.

## What this game asks for

Measured from the 210 `ROL ` resources in `ATLANTIS.001` (XOR-0x69, SCUMM v5
blocks, each an SMF behind an `MDhd` header): **191.8 minutes of MT-32 music,
175.4 minutes of it sounding.** Weighted by real time across the set's 368
distinct tempo changes, which span 24 to 400 BPM - tick-weighting is not
time-weighting here and gives different answers.

### Polyphony

Concurrent notes on the melodic parts, as a share of all playing time:

| Notes | Share | Cumulative |
| ---: | ---: | ---: |
| 0 | 9.6% | 9.6% |
| 1 | 7.1% | 16.7% |
| 2 | 17.2% | 33.9% |
| 3 | 11.2% | 45.0% |
| 4 | 10.0% | 55.0% |
| 5 | 8.6% | 63.6% |
| 6 | 8.7% | 72.4% |
| 7 | 6.9% | 79.3% |
| 8 | 5.5% | 84.7% |

Mean while anything sounds: **5.63 notes**. Peak per cue: median 9, maximum 20.

### Parts, programs, timbres

- Channels used: 1-8 and 10 - exactly the MT-32's eight melodic parts plus
  rhythm. Nothing outside the model.
- **1-4 parts are active 77.8%** of the time; five or more only 13.6%.
- 68 distinct program numbers across the set.
- **No custom timbre upload.** The 3,515 SysEx messages in the music are all
  iMUSE's own `0x7D` markers, not Roland `DT1` writes, and iMUSE's part setup
  passes a program number
  (`part->_instrument.program(buf[8], 0, player->_isMT32)` in
  `engines/scumm/imuse/sysex_scumm.cpp`). `ROLAND.IMS` contains no Roland
  SysEx header and no timbre-shaped record, and it is **not packed** - payload
  entropy 6.75 bits/byte and no LZEXE, PKLITE, DIET or EXEPACK signature - so
  there is no hidden timbre table either. The game plays the **factory
  patches**.

### The driver files

`ROLAND.IMS` is byte-identical to `ROL_330.IMS`; `ROL_332`, `ROL_334` and
`ROL_336` each differ in exactly 12 bytes. Ten of those are `mov dx, imm16`
operands stepping the MPU-401 base from `0x0330`/`0x0331` to
`0x0332`/`0x0333`, `0x0334`/`0x0335` and `0x0336`/`0x0337`; the other two are
the MZ header checksum at offset 0x12. They are **I/O port variants of one
driver**, not variants for different Roland models. The driver's only other
notable content is a 20-character string, `"Lucasfilm Games     "` - the
MT-32's LCD width - and part/channel tables.

### MT-32 or CM-32L: a plain MT-32 is enough

This was an open decision in
[`mt32-ground-truth.md`](/Users/saschaspringer/Work/F030MT32/docs/mt32-ground-truth.md).
The game's rhythm part settles it. Munt's `RhythmPart::noteOn` accepts keys
24-108 but records that anything **above 87 is invalid on an MT-32** and works
only on a CM-32L/LAPC-I. Across all 8,540 rhythm note-ons in the game:

| Quantity | Value |
| --- | --- |
| Rhythm key range used | **24 - 82** |
| Distinct rhythm keys | 38 |
| Note-ons above key 87 (CM-32L only) | **0** |
| Note-ons below key 24 (invalid on both) | 0 |

So the port can target the MT-32 and requirement 2 shrinks accordingly: the
**512 KiB** MT-32 PCM ROM rather than the CM-32L's 1 MiB. It does not shrink
the DSP-addressability problem, which is about the 32,768-word SRAM, not the
host side.

### The rhythm part

The rhythm part is **silent 93.5%** of the time, active 6.5%, and averages
1.61 notes when active.

That is a useful result, because rhythm notes are PCM partials and the PCM ROM
is the one obstacle the budget document calls absolute. **Caveat:** LA timbres
use PCM attack transients in melodic partials too, so 6.5% bounds *rhythm*
PCM demand, not total PCM demand. Which factory patches use PCM partials
cannot be known without the control ROM.

## What ScummVM sends the device

`IMuseDriver_MT32::initDevice()` in `engines/scumm/imuse/drivers/midi.cpp` is
the exact SysEx surface a Falcon synthesis backend has to accept, because it is
what ScummVM will send it. Fate of Atlantis takes the **old** driver path -
`newSystem = (_game.id == GID_SAMNMAX)` in `scumm.cpp:2566` is false for
INDY4 - which means 9 MIDI channels, no GM program remapping
(`_noProgramTracking`), and the rhythm setup below is sent.

Addresses below are the real 7-bit MT-32 form; ScummVM packs them into a
21-bit integer and `sendMT32Sysex` splits and checksums them behind the
standard `41 10 16 12` header.

| Address | What | Data |
| --- | --- | --- |
| `20 00 00` | Display message | `"    ScummVM 2.9.0   "`, 20 chars |
| `7F 00 00` | All-parameters reset | none, then a 250 ms wait |
| `10 00 00` | System area | 23 bytes, below |
| `03 01 10` | Rhythm setup | 11 entries x 4 bytes, keys 24-34 |
| `05 00 04` + `i*8` | Patch memory, bender range | `0x10` for **all 128 patches**, 5 ms apart |

The 23-byte system-area write is the important one. Decoded against the MT-32
system area layout:

| Field | Bytes | Value |
| --- | --- | --- |
| Master tune | 1 | `0x40` |
| **Reverb mode** | 1 | **`0x00` = Room** |
| Reverb time | 1 | `0x04` |
| Reverb level | 1 | `0x04` |
| **Partial reserve** | 9 | **`4,4,4,4,4,4,4,4,0`** - four per melodic part, none for rhythm, **summing to exactly 32** |
| MIDI channel map | 9 | `1..9` |
| Master volume | 1 | `0x64` = 100 |

Two consequences for this port:

- **The measured reverb is the right one.** `la32-budget.md` charges 83
  cycles/frame for bit-exact *room* reverb, and room is exactly the mode
  ScummVM configures. That figure is not a placeholder for this game.
- **The partial allocation policy is known and is not iMUSE's.** Each melodic
  part is guaranteed four partials and the rhythm part is guaranteed none,
  which is what Munt's `PartialManager` then enforces. A reduced-pool Falcon
  has to decide what to do with a reserve table that assumes 32; scaling it is
  a design decision with an audible consequence, not a detail.

### iMUSE's own priorities will not order the degradation

Decoding all 844 part-allocation messages (iMUSE SysEx code 0, nibble-packed
as `Player::decode_sysex_bytes` unpacks them):

| Field | Finding |
| --- | --- |
| Priority adjustment | **726 of 844 are 0**; the rest are 1, 2, 4, or negative (`0xFD` = -3, `0xFF` = -1, `0x7E`/`0x7F`) |
| Volume | **839 of 844 are 127** |
| Reverb enabled | **830 of 844** |
| Percussion parts | 102 |
| Channels | 0-7 and 9 |

So the game does not express a useful ordering of which parts matter. Under a
pool smaller than 32 the degradation order comes from the MT-32's own reserve
table and `PartialManager`, not from the score. And reverb is effectively
mandatory: it is on in 98% of part allocations, so the 83 cycles/frame - 17% of
the whole DSP budget - cannot be reclaimed by turning it off. That makes the
half-rate reverb lever (open lever 3, worth about 45 cycles) directly relevant
rather than speculative.

### The timbre question is smaller than it looks

Weighted by note-seconds across all 210 cues, program usage is highly
concentrated:

| Programs | Share of note-seconds |
| ---: | ---: |
| top 5 | **59.7%** |
| top 10 | 75.9% |
| top 20 | 89.6% |
| top 30 | 95.4% |

Program 50 alone is 27.6%, then 92 (12.0%), 32 (10.0%), 0 (5.5%) and 36
(4.5%). 68 programs appear in total.

This matters for the open partials-per-note question: it is really a question
about **ten to twenty timbres**, not 128. Once a control ROM is available, the
weighted average follows from those timbres' partial-mute fields, and a port
that wanted to hand-optimise a few kernels would know which ones. Patch names
also live in the control ROM, so the programs cannot be named here.

## What the Falcon supplies

From [`la32-budget.md`](/Users/saschaspringer/Work/F030MT32/docs/la32-budget.md):

| Quantity | Value |
| --- | --- |
| DSP instruction rate | 16,042,494 cycles/s |
| Codec rate (prescale 2) | 32,779.947916 Hz |
| Cycles per output frame | 489.40 |
| Reverb + transport | 83 + 14.81 = 97.81 |
| **Left for partials and controls** | **391.59** |

Floored homogeneous partial counts:

| Kernel | Settled square | Settled saw | Moving square | Moving saw |
| --- | ---: | ---: | ---: | ---: |
| Exact | 5 | 4 | 4 | 3 |
| Approximate | 8 | 7 | 5 | 5 |

Plus **2 PCM partials** on the 68030 as the planning figure (3 fit the
arithmetic, leaving no room for MIDI and control work). A slot in one pool
cannot pay for a slot in the other. That document's own summary: **3-5 exact
or 5-8 approximate synth partials plus up to 2 PCM**, or "roughly 2-4 typical
notes".

## The gap: measured

The conversion is no longer a hypothesis. Munt, control ROM v1.07 plus the
MT-32 PCM ROM, over all 210 cues:

| Quantity | Value |
| --- | ---: |
| Rendered | 209.7 min, 170.9 min sounding |
| Mean partials while sounding | **12.70** of 32 |
| Mean notes while sounding | 5.67 |
| **Partials per note** | **2.24** |
| Peak partials / peak notes | 32 / 24 |
| Munt itself at the 32-partial ceiling | 0.4% of the time |

Share of playing time that fits each measured Falcon budget, i.e. where nothing
is stolen:

| Partial budget | Coverage |
| --- | ---: |
| 3 (exact, moving saw) | 30.8% |
| 4 (exact, settled saw) | 36.2% |
| 5 (exact, settled square) | 38.4% |
| 5 (approximate, moving) | 38.4% |
| 7 (approximate, settled saw) | 45.2% |
| **8 (approximate, settled square)** | **50.1%** |

**The best measured Falcon configuration covers just under half the game's
music.** The other half steals voices, which is by definition not the faithful
target. The original hardware is not at 100% either - it sits at its own
ceiling 0.4% of the time on this material - but 0.4% and 50% are different
kinds of statement.

### Per-timbre costs

`mt32-partials --probe` plays one note per program and reads the pool, so these
are direct measurements, with names from the control ROM:

| Program | Name | Partials | Share of melodic note-seconds |
| ---: | --- | ---: | ---: |
| 50 | Str Sect 3 | 2 | 28.1% |
| 92 | Fr Horn 1 | 3 | 12.3% |
| 32 | Fantasy | 3 | 10.2% |
| 0 | AcouPiano1 | 4 | 5.7% |
| 36 | Soundtrack | 4 | 4.6% |
| 93 | Fr Horn 2 | 2 | 4.4% |
| 48 | Str Sect 1 | 4 | 4.1% |
| 82 | Clarinet 1 | 3 | 3.4% |
| 57 | Harp 1 | 3 | 2.6% |
| 12 | Pipe Org 1 | 3 | 2.1% |

Across all 128 programs: 17 use one partial, 57 use two, 34 use three, 20 use
four. Weighted by this game's usage, two-partial timbres carry 45.0% of melodic
note-seconds, three-partial 38.8%, four-partial 15.3%, one-partial 1.0%.

So a port cannot buy much by special-casing: the cheapest timbre that matters
still costs two partials, and the single most-used timbre in the game
(`Str Sect 3`, 28.1% of note-seconds) is one of them.

### Why the two estimates differ, and why both are right

Weighting the per-timbre costs above by usage gives **2.68** partials per
melodic note, against Munt's measured **2.24**. Both are correct measurements
of different things:

- Munt's figure includes the rhythm part, whose notes are cheaper.
- `getPlayingNotes` counts a note until its **last partial** deactivates
  (`Poly::isActive` is `state != POLY_Inactive`), so release tails count as
  sounding notes after their partials have gone - which lowers the ratio.
- The probe measures a fresh note's **peak**, before any partial has finished.

They bracket the answer at two to three partials per note.

### Cross-checks

- Munt's mean note count, **5.67**, matches the **5.63** measured independently
  from the raw MIDI by `foa-mt32-demand.py`. Two unrelated methods, 0.7% apart.
- Munt ignores iMUSE's `0x7D` markers, as it should - they address the
  sequencer, not the device. That left programs arriving only via real `0xC0`
  messages, which covered **96.42%** of melodic note-ons. The 3.58% gap had a
  program waiting in the iMUSE allocation record in 98.5% of cases, and the
  real driver forwards it (`part->_instrument.program(buf[8], ...)`), so the
  exporter now does too. Closing it moved partials per note from 2.25 to 2.24
  and coverage up by about two points - small, but the corrected figures are
  the ones above.

## Requirements, then

For a faithful implementation on the stock 16 MHz / 14 MB / DSP56001 Falcon:

1. **The full 32-partial pool.** The target document is explicit that reducing
   the pool does not satisfy it. Against the budget above this is the 4.91x
   (square) to 6.22x (saw) overrun already recorded, before any game work.
2. **A PCM ROM answer.** 262,144 samples against 32,768 words of DSP SRAM.
   14 MB of host RAM holds the ROM comfortably; the DSP cannot address it. Any
   design that streams or caches must have its cache coverage, refill cost,
   transfer cost and worst-case correctness measured, which has not been done.
3. **Control ROM tables resident on the 68030.** 64 KiB, trivial against
   14 MB, but a hard dependency - and it cannot ship with the port.
4. **One owner for the sound hardware.** ScummVM's
   `backends/mixer/atari/atari-mixer.cpp` owns uSound, the DMA buffers and
   Timer A; F030MT32's transport configures the same hardware. A synthesis
   backend has to live behind ScummVM's MIDI driver interface
   (`audio/softsynth/mt32.cpp` is the shape to preserve) and share that owner.
   Two sound-owning programs is not an integration.
5. **Host budget for the game itself.** The stripped Falcon ScummVM build
   leaves roughly 9.3 MB of the 14 MB free, but memory is not the constraint -
   cycles are, and the game, its graphics and CD speech have not been profiled
   against a running synthesiser.
6. **The rhythm part is cheap here.** At 6.5% activity and 1.61 mean notes,
   two PCM partials cover the rhythm part almost always. This is the one place
   the measured demand lands comfortably inside the measured supply.

## What is still open

Evidence item 2 in
[`scummvm-target.md`](/Users/saschaspringer/Work/F030MT32/docs/scummvm-target.md)
is now satisfied for the music: the game's partial demand is measured, not
projected. What remains:

- **Whether melodic partials read PCM waves.** This decides whether the PCM ROM
  obstacle sits on 6.5% of the music (the rhythm part alone) or most of it.
  Munt's public API exposes partial *state* but not partial *waveform source*,
  so this needs either a small patch to the vendored Munt or parsing the
  control ROM's timbre tables through `ControlROMMap`. It is the last question
  that needs neither a Falcon nor hardware.
- **Concurrent load**, evidence item 3: game logic, graphics and CD speech
  running against the synthesiser. Nothing here touches it.
- **Verification on physical hardware**, evidence item 4.

## What this document does not establish

- Any statement about total PCM demand beyond the rhythm part.
- Anything about concurrent load: these are the music's demands in isolation,
  with no game logic, graphics, CD speech or sound effects running.
- **Concurrent cues.** iMUSE has 8 players and 32 parts
  (`imuse_internal.h:468`), so music and sound effects can sound together and
  compete for the device's 9 parts. Every polyphony figure here is measured
  per cue in isolation and is therefore a **lower bound** on what the device
  sees in play. 50 of the 210 resources are under 5 s and 84 use two parts or
  fewer, which is the shape of stingers and effects rather than music, so
  overlap is likely rather than hypothetical.
- Anything measured on physical hardware.

## The harness is ready

[`tools/mt32-partials`](../tools/mt32-partials/) builds against the vendored
Munt and takes the measurement described above. It runs as soon as a ROM pair
is dropped in:

```sh
cd devtools/atari-falcon030/tools/mt32-partials
make
make run ROMS=/path/to/roms
```

Its cue export and parsing are verified without a ROM
(`--check-cues`: 210 cues, 176,500 events, OK). Its synthesis half has not
been run, because no ROM is available in this environment.
