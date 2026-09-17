# Faithful MT-32 for Fate of Atlantis on a stock Falcon: the requirements

Historical live-synthesis requirements and offline resource-demand analysis.
The [polyphony assessment](mt32-polyphony-options.md) corrects the earlier
capacity and CPU-headroom claims. Current work is the
[FCM1 compiled MIDI/iMUSE and instrument-data prototype](../tools/foa-compiled-music/README.md),
with live synthesis as the target and no prerendered soundtrack.

**2026-09-14 correction:** directory-indexed extraction finds 204 resources,
257 tracks, 3,417 iMUSE operations and 69 custom-instrument events containing
52 distinct timbres. The earlier scanner counted 210 physical copies, truncated
ROL payloads by eight bytes and missed custom instruments. Historical demand,
duration and usage statistics below therefore need recomputation through real
iMUSE; they are not valid whole-game requirements or capacity predictions.
The measured component costs remain separate evidence.

Component costs are cited from the F030MT32 project
(`/Users/saschaspringer/Work/F030MT32/docs/`); complete renderer capacities are
planning estimates, not achieved live polyphony. Resource demand is analyzed by
[`tools/foa-mt32-demand.py`](../tools/foa-mt32-demand.py). Anything neither
measured nor cited is marked unverified.

## Historical demand estimate (superseded extraction)

**The offline cue analysis estimates 2.24 LA partials per note. Aggregate
demand is at most eight partials for 50.1% of the rendered time, including
silence. This is not measured Falcon playback coverage.** Mean demand while
music sounds in that analysis is 12.70 of the MT-32's 32 partials. The quoted
3–5 exact or 5–8 approximate Falcon synth partials are component-based
estimates; there is no demonstrated complete live renderer at those counts.

**27.2% of that demand is PCM partials**, which on this port run on the 68030
with a budget of two, against a demand of about 3.5. Both pools are over
budget and a slot in one cannot pay for a slot in the other.

An MT-32 control v1.07 and PCM ROM pair is now available locally and was used
for the Munt analysis. ROM availability is no longer a blocker in this
workspace; DSP addressability and transfer costs remain constraints for live
synthesis. Rhythm is active in 6.5% of the analyzed resource time.

These resource measurements do not cover live iMUSE state. The raw cue export
does not implement all part setup, hooks, jumps, fades or overlapping players;
it must not be used as a faithful soundtrack renderer. Capture after the real
iMUSE and MT-32 driver instead. The new reference tool does this for one
opening sequence, with complete 32-partial Munt synthesis and reverb.

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
- **Custom instruments are required.** Correct indexed parsing finds 69
  Roland-form custom-timbre events with 52 distinct 246-byte definitions,
  in addition to 3,417 iMUSE `0x7D` operations. `Player::sysEx` in
  `engines/scumm/imuse/imuse_player.cpp` assigns these to the logical iMUSE
  part encoded in the message, then applies them through the allocated
  channel. They must not be treated as immediate writes to a fixed hardware
  part. FCM1 preserves their original bytes and Munt-normalized definitions.
  Driver-file inspection did not justify the earlier factory-only conclusion.

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

In the raw resource analysis the rhythm part is **silent 93.5%** of the time, active 6.5%, and averages
1.61 notes when active.

That is a useful result, because rhythm notes are PCM partials. **Caveat:** LA timbres
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

### Historical program-number concentration (not custom-timbre coverage)

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

These program-number statistics do not bound the required instrument set:
custom timbres and live iMUSE part state were missing from this analysis.
FCM1 currently retains all 128 factory melodic timbres, 30 rhythm timbres and
52 game custom timbres, rather than pruning by these old usage figures.

## What the Falcon supplies

From [`la32-budget.md`](/Users/saschaspringer/Work/F030MT32/docs/la32-budget.md):

| Quantity | Value |
| --- | --- |
| DSP instruction rate | 16,042,494 cycles/s |
| Codec rate (prescale 2) | 32,779.947916 Hz |
| Cycles per output frame | 489.40 |
| Reverb + transport | 83 + 14.81 = 97.81 |
| **Left for partials and controls** | **391.59** |

Floored homogeneous partial counts projected from measured component costs:

| Kernel | Settled square | Settled saw | Moving square | Moving saw |
| --- | ---: | ---: | ---: | ---: |
| Exact | 5 | 4 | 4 | 3 |
| Approximate | 8 | 7 | 5 | 5 |

Plus **2 PCM partials** on the 68030 as the planning figure (3 fit the
arithmetic, leaving no room for MIDI and control work). A slot in one pool
cannot pay for a slot in the other. That document's own summary: **3-5 exact
or 5-8 approximate synth partials plus up to 2 PCM**, or "roughly 2-4 typical
notes".

## Offline demand compared with projected capacity

Munt, control ROM v1.07 plus the MT-32 PCM ROM, over all 210 raw exported
cues. These are demand-harness results, subject to the iMUSE omissions above:

| Quantity | Value |
| --- | ---: |
| Rendered | 209.7 min, 170.9 min sounding |
| Mean partials while sounding | **12.70** of 32 |
| Mean notes while sounding | 5.67 |
| **Partials per note** | **2.24** |
| Peak partials / peak notes | 32 / 24 |
| Munt itself at the 32-partial ceiling | 0.4% of the time |

Share of total rendered time, including silence, below each aggregate
partial-count threshold. This does not test separate PCM/synth pools,
scheduling or voice stealing in a Falcon renderer:

| Partial budget | Coverage |
| --- | ---: |
| 3 (exact, moving saw) | 30.8% |
| 4 (exact, settled saw) | 36.2% |
| 5 (exact, settled square) | 38.4% |
| 5 (approximate, moving) | 38.4% |
| 7 (approximate, settled saw) | 45.2% |
| **8 (approximate, settled square)** | **50.1%** |

**50.1% is an offline threshold statistic, not achieved Falcon coverage.**
Likewise, Munt reaching 32 active partials for 0.4% of the render does not
by itself count stolen notes. Faithful playback includes the original
32-partial allocation behavior, including any stealing the MT-32 would do.

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

### PCM is 27% of the demand, not 6.5%

The rhythm part is active only 6.5% of the time, but LA timbres read PCM attack
transients in melodic partials too, and this game does so heavily.
[`tools/foa-mt32-pcm.py`](../tools/foa-mt32-pcm.py) reads the control ROM's
group A and B timbre maps, takes each timbre's `partialMute` and its two
partial-structure values, and applies Munt's `PartialStruct` table (`Part.cpp`)
to decide which partial of a pair reads PCM.

| Quantity | Value |
| --- | ---: |
| Programs using at least one PCM partial | **84 of 128** |
| PCM partials per melodic note | **0.73** |
| **PCM share of melodic partial demand** | **27.2%** |
| Melodic note-seconds on a PCM-using timbre | **55.0%** |

Cross-validated against `mt32-partials --probe`, which measures partial counts
by playing a note: **127 of 128** programs agree on both count and name. The
one exception is program 67, `Elec Bass2`, where the ROM marks two partials
sounding and the probe measures one - consistent with a partial whose TVA
gives it no level at the probed key and velocity, not with a parsing error.

This is the answer to the question that was left open, and it is the
unfavourable one for live synthesis. **PCM access matters for melodic music
as well as rhythm.** The top timbre in the
game (`Str Sect 3`, 28.1% of note-seconds) is PCM-free, but the next three -
`Fr Horn 1`, `Fantasy`, `AcouPiano1` - are not.

### Both pools are over budget, and they cannot trade

As a rough planning calculation, applying the 27.2% melodic timbre split
to the 12.70 aggregate mean gives the following. This mixes two weightings
and includes neither actual per-pool peaks nor a measured live capacity:

| Pool | Demand | Falcon supply | Over by |
| --- | ---: | ---: | ---: |
| LA synth partials (DSP56001) | ~9.2 | 3-5 exact, 5-8 approximate | ~1.2-3x |
| PCM partials (68030) | ~3.5 | **2** | ~1.7x |

`la32-budget.md` is explicit that "a partial slot in one pool cannot pay for a
partial in the other". So the two shortfalls compound rather than average: a
design that solved the DSP side entirely would still be short on PCM for this
game's music, and vice versa.

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
   leaves roughly 9.3 MB arithmetically for heap, resources and buffers;
   memory high-water is unmeasured. The game, its graphics and CD speech
   have not been profiled against a running synthesiser.
6. **The rhythm part is cheap here.** At 6.5% activity and 1.61 mean notes,
   two PCM partials exceed that mean in the resource analysis. This is not
   a worst-case guarantee and the two-partial supply remains a planning figure.

## What is still open

Evidence item 2 in
[`scummvm-target.md`](/Users/saschaspringer/Work/F030MT32/docs/scummvm-target.md)
has useful offline resource measurements, but actual post-iMUSE demand,
overlapping players and interactive state still need coverage. What remains:

- **Concurrent load**, evidence item 3: game logic, graphics and CD speech
  running against the synthesiser. Nothing here touches it.
- **Verification on physical hardware**, evidence item 4.

## What this document does not establish


- Anything about concurrent load: these are the music's demands in isolation,
  with no game logic, graphics, CD speech or sound effects running.
- **Concurrent cues.** iMUSE has 8 players and 32 parts
  (`imuse_internal.h:468`), so music and sound effects can sound together and
  compete for the device's 9 parts. Every polyphony figure here is measured
  per cue in isolation. Because live iMUSE can also mute parts, jump and stop
  cues, these statistics are not a rigorous lower bound on live demand.
  50 of the 210 resources are under 5 s and 84 use two parts or
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

Its cue export and parsing were verified without a ROM
(`--check-cues`: 210 cues, 176,500 events, OK). Its synthesis half was then
run with the local ROM pair, producing the historical statistics above.
Use the new post-iMUSE capture tool for reference audio; this harness does
not implement the complete sequencer semantics.
