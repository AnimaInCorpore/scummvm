# mt32-partials

Measures how many LA32 partials Fate of Atlantis actually asks an MT-32 for,
by playing the game's own music through Munt and sampling the partial pool.

This exists to settle one number. The Falcon DSP budget in
`F030MT32/docs/la32-budget.md` is denominated in **partials**; the demand
measurement in [`../../docs/mt32-requirements.md`](../../docs/mt32-requirements.md)
is in **notes** (mean 5.63 concurrent while sounding). The conversion between
them - partials per note for the MT-32's factory patches - is unmeasured, and
it is what decides whether the Falcon covers 55-85% of the music or 17-45%.

## Running it

```sh
make                                   # build against the vendored Munt
make run ROMS=/path/to/roms            # measure
```

`MUNT` defaults to `../../../../../F030MT32/third_party/munt`, matching how
`devtools/atari-ste` finds Hatari and the cross toolchain. `FOA_DATA_DIR`
points at the game data if it is not at `assets/atlantis-cd`. `MAX_CUES=20`
shortens a trial run.

`./build/mt32-partials --list-roms` prints every image this Munt recognises
with its size and SHA-1, so a dump you make yourself can be checked before use:
if it appears in that table it will be accepted, and if it does not it will be
rejected rather than mispaired.

**A ROM pair is required and cannot ship here.** Supply your own MT-32 (or
CM-32L) control ROM and PCM ROM as complete images; Munt identifies them by
size and SHA-1, and the harness rejects half and mux images rather than
mispairing them silently. These are Roland's firmware and PCM data and are not
redistributable; dump them from hardware you own. `F030MT32/roms/` does not exist in that checkout, so
this is the one thing standing between the harness and an answer.

## What it reports

- the time-weighted distribution of active partials over all 210 cues, and the
  peak;
- mean partials and notes while sounding, and **partials per note**;
- how much of the time Munt itself sits at the 32-partial ceiling, i.e. where
  the original hardware steals voices on this material;
- the share of playing time that fits each measured Falcon partial budget
  (3, 4, 5, 7 and 8 partials from `la32-budget.md`).

## How it works

`foa-mt32-demand.py --export` writes one `cue-NNN.ev` per cue: absolute
microseconds and raw MIDI bytes, tempo already resolved from the set's 368
tempo changes. The harness replays those into a fresh `MT32Emu::Synth` per cue
(the game reinitialises the device between cues, and a shared synth would carry
timbre and part state across them), renders in 64-frame blocks at
`AnalogOutputMode_DIGITAL_ONLY` so the rate is the MT-32's own 32 kHz with no
analog resampling, and after each block reads `getPartialStates()` and
`getPlayingNotes()`. Both counts therefore come from Munt, not from the
harness's own bookkeeping.

Munt is used at build time only. Nothing here goes near the Falcon binary.

## Result

Measured with MT-32 control v1.07 + PCM ROM over all 210 cues:
**2.24 partials per note**, mean 12.70 of 32 partials while sounding, peak 32,
Munt at its own ceiling 0.4% of the time. Coverage of playing time by Falcon
budget: 30.8% at 3 partials, 36.2% at 4, 38.4% at 5, 45.2% at 7, 50.1% at 8.

`--probe <roms-dir>` gives the per-timbre breakdown behind that: one note per
program, partial count and name from the control ROM. Of 128 programs, 17 cost
one partial, 57 two, 34 three, 20 four.

## Verification status

- `./build/mt32-partials --check-cues build/cues` needs no ROM and checks that
  every cue parses, timestamps rise, and every message is one Munt accepts.
  Current result: **210 cues, 176,500 events (172,985 voice, 3,515 sysex),
  longest cue 792.8 s, OK.** The sysex count matches the Python side exactly.
- The missing-ROM path reports what is missing and exits 1.
- `--list-roms` prints the 16-image catalogue and needs no ROM.
- The synthesis half has now been run over the full set twice, once before and
  once after the allocation-program fix below.
- Munt logs `playSysexWithoutFraming: Header not intended for this device
  manufacturer: 7d ...` throughout a run. That is correct and harmless: the
  `0x7D` messages address iMUSE, not the MT-32.
- Because Munt ignores those, programs reach it only via real `0xC0` messages,
  which covered 96.42% of melodic note-ons. The exporter now also forwards the
  program carried in iMUSE part-allocation records, as the real driver does.
  That moved the result from 2.25 to 2.24 partials per note and coverage up
  about two points.
