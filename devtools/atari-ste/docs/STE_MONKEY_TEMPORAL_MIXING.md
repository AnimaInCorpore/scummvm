# STE temporal colour mixing — 2026-09-11

**The STE port can show room 28 without the per-line palette raster.** Each VGA
colour becomes a pair of entries of one fixed 16-colour palette. Two screen
buffers hold the two colours of every pair and the VBL hook alternates them
every 50 Hz field, so the eye sees their linear-light average. On eight frames
captured from the running game, this scores 4.52 mean squared Oklab error per
pixel (3.11 after a 2×2 blur) against the original VGA colours. Full Spectrum
512 with 48 colours per line scores 20.03 (9.93), because it cannot leave the
12-bit colour gamut. The raster it replaces costs a fixed 46% of the CPU
([measurement](STE_MONKEY_ROOM28_MEASUREMENT.md)).

This removes the display's CPU cost; it does not reach 10 fps. The engine phase
alone costs 6.25M cycles per frame, 7.8× a whole 10 fps frame.

## How it works

- **Pairs.** A table maps every source colour to two palette entries. Their
  Oklab lightness difference is limited (ΔL ≤ 0.20 by default) because the
  25 Hz lightness alternation is what the eye sees as flicker.
- **Checkerboard.** A pixel shows its pair's first colour in field 0 when
  `(x + y)` is even, and the second colour when it is odd; field 1 swaps them.
  Every area therefore shows both colours in both fields and its brightness
  stays constant; only single pixels alternate.
- **Verb bar.** Lines 144 to 199 hold 4 colours. They get their own palette,
  which a single Timer B one-shot loads after line 143. Slot 0 is black in every
  palette and line 144 is black, so an interrupt one line late stays invisible.
- **Per buffer.** Each of the three triple-buffering screens has a second field.
  The 256 KiB video RAM pool is full, so the second fields come from the heap
  (every STE byte is displayable).
- **Conversion.** A source pixel costs two table lookups per field on the
  68000. A palette change rebuilds the per-colour tables; colours the table was
  not computed for fall back to the nearest pair.

## Using it in the port

Mixing is the port's default display. Without configuration it loads
`MIX\DUAL20.BIN` from the current directory (the program's folder when started
from the desktop), whose two palettes alternate every field. To choose another
table or pattern, set them in `[scummvm]` of `SCUMMVM.INI` (the graphics manager
reads configuration before the game section is active):

```ini
ste_mix_lut=C:\SCUMMVM\MIX\PAIR20.BIN
ste_mix_pattern=checker
```

One-palette tables use the checkerboard unless `ste_mix_pattern` is `alternate`
or `static`. Two-palette tables always alternate; any other pattern set for them
logs a warning. An empty `ste_mix_lut`, or a table that cannot be loaded, keeps
the per-line raster.

A table holds one room's colours, so `R<room>.BIN` next to the default table is
installed while that room is on screen (`R064.BIN` for room 64). Rooms without
one use the default table.
`devtools/atari-ste/tools/scumm-ste-room-tables.mjs` builds a game's set from
the game files.

The room comes from `ScummEngine::startScene` through
`atari_ste_scene_room()`; everything else is in the backend. Changed files in
`scummvm-ste-scene/backends`: `graphics/atari/atari-ste-scene.*`
(table loading, fallback and two-field conversion), `atari-ste-raster.*` (field
alternation in the VBL hook and the Timer B verb-bar switch), `atari-screen.*`
(the second field and the cursor rectangle per buffer), `atari-cursor.*` (the
cursor in the engine's frame), `atari-graphics.cpp` (configuration,
presentation, cursor, capture pointers) and `platform/atari/ste-benchmark.h`
(`atari_ste_last_source` and `atari_ste_last_palette` for debugger captures).
Frames captured from a running port therefore carry the cursor where it stood.

MiNTLib declares `_RGB` components as signed `char`. Palette bytes of 128 or
more must be cast before comparing them; this bug once sent every bright colour
to the fallback and turned Guybrush blue.

## Building tables

Tables must come from frames of the running game: the game sets palette entries
2, 3, 6 and most of 144 onward itself, so tables from the room's `CLUT` miscolour
actors.

```sh
node devtools/atari-ste/tools/scumm-ste-frame-capture.mjs --out devtools/atari-ste/monkey-bar/room28-capture
node devtools/atari-ste/tools/monkey-flicker-compare.mjs --capture devtools/atari-ste/monkey-bar/room28-capture --split 144 --dl 0.1,0.15,0.2,0.25,0.3 --sheet-dl 0.2 --out devtools/atari-ste/monkey-bar/room28-capture/flicker-compare-split
```

The capture tool breaks at the renderer marker after each requested VBL and saves
the engine's finished 320×200 frame and live palette. The benchmark hooks behind
that marker only run in room 28; for other rooms and games (`--game`,
`--boot-param`, `--room`) it breaks at the entry of
`AtariSteSceneRenderer::convert` and records the rooms entered and the costumes
drawn in each. `scumm-scene-colours.mjs` turns those costumes, the room's own
ones, its background and its objects into the colour list `--union` fits the
palettes to; `scumm-mix-table-check.mjs` scores a finished table against it.
The evaluator writes
`lut-pair16-dl*.bin` and `lut-dual16-dl*.bin`; copy them to `C:\SCUMMVM\MIX`
under 8.3 names (`PAIR20.BIN`, `DUAL20.BIN`). Table layout: palette words, slot
pairs per region, then the 768-byte VGA palette they were computed for; see
`lutFormat` in `flicker-metrics.json`.

## Results on the captured frames

Mean squared Oklab error against the original VGA colours (×127 scale), area
flicker as the lightness difference between fields after a 2×2 blur, room
colours kept out of 126:

| Strategy | Pixel | Blurred | Area flicker mean / p95 | Room colours kept |
|---|---:|---:|---:|---:|
| RGB12 nearest, no palette limit | 19.68 | 9.79 | 0 | — |
| Per-line 16 colours | 21.84 | 10.94 | 0 | — |
| Full Spectrum 512 | 20.03 | 9.93 | 0 | — |
| One palette, checkerboard, ΔL ≤ 0.10, no split | 9.14 | 6.63 | 0.005 / 0.028 | 32 of 129 |
| One palette, checkerboard, ΔL ≤ 0.10, split | 6.95 | 5.01 | 0.006 / 0.031 | 35 |
| One palette, checkerboard, ΔL ≤ 0.15, split | 5.76 | 3.89 | 0.006 / 0.034 | 46 |
| **One palette, checkerboard, ΔL ≤ 0.20, split** | **4.52** | **3.11** | **0.009 / 0.045** | **53** |
| One palette, checkerboard, ΔL ≤ 0.30, split | 4.11 | 2.83 | 0.013 / 0.066 | 57 |
| Two palettes, alternating, ΔL ≤ 0.20, split | 2.81 | 1.75 | 0.080 / 0.177 | 68 |

**Verified in Hatari** with the ΔL ≤ 0.20 split table: after room entry all 256
colours come from the table, both displayed fields match it pixel for pixel
(92,160 room and 35,840 verb-bar pixels), and the verb-bar palette switches
exactly at line 144.

## Fate of Atlantis, room 64

The same pipeline built a table for the Algiers bazaar (room 64), the first
room the game plays after the logo, from eight frames captured with
`--game atlantis --boot-param 9554 --room 64`. Room lines hold 174 VGA colours
and the verb bar 17. Split table, same optimiser defaults, fitted to the frames
only (`--union` changes this; see below and
[the Atlantis notes](STE_ATLANTIS_DUAL20.md#what-the-palettes-are-fitted-to)):

| Strategy | Pixel | Blurred | Area flicker mean / p95 | Colours kept |
|---|---:|---:|---:|---:|
| Per-line 16 colours | 10.09 | 4.89 | 0 | — |
| Full Spectrum 512 | 7.13 | 3.37 | 0 | — |
| One palette, checkerboard, ΔL ≤ 0.20 | 4.82 | 2.91 | 0.011 / 0.048 | 83 |
| **Two palettes, alternating, ΔL ≤ 0.20** | **2.22** | **1.19** | **0.058 / 0.136** | **112** |
| Two palettes, alternating, ΔL ≤ 0.25 | 1.97 | 1.06 | 0.069 / 0.173 | 121 |
| Any two RGB12 colours (no palette) | 0.76 | 0.48 | 0.048 / 0.094 | 188 |

`lut-dual16-dl0.20.bin` installed as `MIX\DUAL20.BIN` is what the port loads
without configuration. The installed table is the `--union` one: fitting the
palettes to the frames alone serves what those frames happened to show, so
`scumm-scene-colours.mjs` lists every colour the room's background, objects and
costumes can produce, and the evaluator gives each of them a floor weight. Over
all 196 indices room 64 can show, that moves the mean squared Oklab error from
25.26 to 12.41 and the colours visibly off from 20 to 8; of the 31 indices only
a costume uses, from 73.67 to 20.60 and from 7 to 3. The price is the error of
the captured frames themselves, 2.54 to 3.37. **Verified in Hatari** on a 4 MiB STE with TOS 2.06: in
room 64 all 256 colours come from the table, the average of two consecutive
fields shows the room's VGA colours, the cursor follows the mouse and clicking a
verb changes the sentence line. The room runs at about 1.7 complete frames per
second (30 VBLs per conversion, `debuglevel=0`), so the game answers but is far
from the 10 fps goal.

## Known limits

- Flicker visibility and the Timer B switch are unverified on real hardware;
  `devtools/atari-ste/monkey-bar/hardware-test` holds a test kit and checklist.
- Fate of Atlantis has a table per room; Monkey Island has one for room 28.
  A room without its own table falls back to nearest pairs from the default
  table's palettes, which loses far more than the room's own table would.
- The mouse cursor is drawn into the engine's frame before it is converted
  (`Cursor::steDraw`), so it needs no colours of its own but is mixed like
  every other pixel: SCUMM V5 draws it in palette entry 15, and each field
  shows one entry of that colour's pair, so a thin crosshair flickers where a
  filled area would not.
- Only 16 palette entries give about 70 allowed pairs at ΔL ≤ 0.20 for 126 room
  colours. Rare or close colours merge: Guybrush's mouth (#f07458) shares the
  pair of his skin (#f88c7c). `--union` with its floor weight is what keeps a
  colour this small in the fit at all; two palettes and a floor of 64 leave
  8 of room 64's 196 colours visibly off.

## Optimiser options kept for later

All measured on the split, ΔL ≤ 0.20 checkerboard table
(`--chroma-weight`, `--error-cap`, `--cap-penalty` in the evaluator):

| Option | Pixel | Blurred | Room colours off by > 0.06 | Mouth | Green bottle |
|---|---:|---:|---:|---|---|
| Defaults | 4.52 | 3.11 | 15 of 127 | merged | correct |
| Colour error weight 0.5 | 5.24 | 3.67 | — | merged | correct; hair olive |
| Colour error weight 0.25 | 6.52 | 4.57 | 14 | visible | tan; pink cast |
| Error cap 0.06, penalty 2,000 | 6.18 | 4.32 | 11 | visible | green; mint shirt |
| Error cap 0.06, penalty 20,000 | 7.92 | 5.70 | 4 | visible | green |

Weighting colours by the square root of their pixel count, or equally, keeps
barely more colours and doubles the pixels that end up wrong.

## Two pairs per colour in a 2×2 pattern: tested, not adopted

`--quad-texture T` shows two pairs per colour: even rows one pair, odd rows the
other, each as a counter-phased checkerboard. Every pixel still alternates
within one pair, so the flicker limit holds, and each 2×2 area averages four
palette entries. The two pair mixes must lie within T in Oklab, which limits the
static row stripes. Split table, ΔL ≤ 0.20; colours kept and off count room and
verb bar together:

| Pattern | Pixel | Blurred | Texture | Colours kept | Colours off by > 0.06 | Mouth |
|---|---:|---:|---:|---:|---:|---|
| Pair checkerboard | 4.52 | 3.11 | 0.032 | 57 | 15 | merged |
| 2×2, mixes within 0.03 | 4.87 | 3.07 | 0.033 | 66 | 16 | merged |
| 2×2, mixes within 0.06 | 7.91 | 3.11 | 0.039 | 89 | 15 | merged |
| 2×2, mixes within 0.10 | 15.14 | 3.47 | 0.046 | 100 | 14 | visible |

More combinations add shades between the palette entries but cannot reach
colours the palette lacks, so the blurred error and the colours that are
visibly off hardly change, while the stripes raise the per-pixel error. The
port does not support this pattern.
