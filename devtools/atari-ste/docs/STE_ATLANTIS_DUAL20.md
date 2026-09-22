# Fate of Atlantis in DUAL20 mode — 2026-09-11

**Indiana Jones and the Fate of Atlantis (DOS CD, English) runs on a stock
8 MHz STE with 4 MiB with the two-palette mixing table as its display, and
answers the mouse.** The table is built from the game itself, the port loads it
without configuration, and the pointer is now part of the converted frame.
Speed is the open point: the Algiers bazaar runs at about 1.7 complete frames
per second.

## Building the table

Three steps: capture frames of the running game, because the engine sets part
of the palette itself; list every colour the room can show, because eight
frames only hold the actors that happened to stand there; then fit the
palettes to both. Room 64, the Algiers bazaar, is the first room the game plays
after the logo; `boot_param 9554` starts there.

```sh
node devtools/atari-ste/tools/scumm-ste-frame-capture.mjs --game atlantis --boot-param 9554 --room 64 \
  --vbls 44000,44400,44800,45200,45600,46000,46400,46800 \
  --out devtools/atari-ste/monkey-bar/atlantis-room64-capture
node devtools/atari-ste/tools/scumm-scene-colours.mjs --game atlantis --room 64 \
  --capture devtools/atari-ste/monkey-bar/atlantis-room64-capture \
  --out devtools/atari-ste/monkey-bar/atlantis-room64-capture/colours.json
node devtools/atari-ste/tools/monkey-flicker-compare.mjs \
  --capture devtools/atari-ste/monkey-bar/atlantis-room64-capture --split 144 \
  --dl 0.15,0.2,0.25 --sheet-dl 0.2 \
  --union devtools/atari-ste/monkey-bar/atlantis-room64-capture/colours.json \
  --out devtools/atari-ste/monkey-bar/atlantis-room64-capture/flicker-compare-union
```

All eight captures are in room 64 and share one palette. The evaluator's
`lut-dual16-dl0.20.bin` is the DUAL20 table; the strategies it compares are in
[the mixing notes](STE_MONKEY_TEMPORAL_MIXING.md#fate-of-atlantis-room-64).

### What the palettes are fitted to

`scumm-scene-colours.mjs` reads the game files and counts, per palette index,
the pixels of the room background over its full width, of every object image,
and of every animation step of every costume the room can present, averaged per
step. Room 64 can show **196 of the 256 indices**, and **31 of them only a
costume uses**. Its costumes are 63, 64 and 131, which the room file owns, plus
2 and 28, which the frame capture saw drawn there (it traces
`ClassicCostumeLoader::loadCostume`, so it names whoever is on screen, not only
whoever changes costume).

The evaluator takes that list with `--union`. Colours the room can show become
targets even when no captured frame held them, and every target of the room
region is worth at least `--colour-floor` pixels per frame, so a costume colour
that covers a few pixels cannot be optimised away. `scumm-mix-table-check.mjs`
reads a finished table back and scores every index the room can show, which is
the measurement that matters here — the evaluator's own pixel error is
dominated by the wall behind the actors.

Room 64, two palettes, ΔL ≤ 0.20, over the same eight captured frames:

| Table | Error on the frames | Every colour the room can show | Visibly off | Only a costume uses | Visibly off |
|---|---:|---:|---:|---:|---:|
| Frames only | **2.54** | 25.26 | 20 of 196 | 73.67 | 7 of 31 |
| Union, floor 64 | 3.37 | **12.41** | **8** | **20.60** | **3** |

The floor is the trade: what the frames happen to show against what the room
can show. Measured with `--dl 0.2` alone, so the numbers differ slightly from
the three-limit run above:

| `--colour-floor` | Error on the frames | Every colour | Visibly off | Only a costume uses |
|---|---:|---:|---:|---:|
| none (frames only) | 2.70 | 25.60 | 21 | 81.70 |
| 4 | 2.64 | 25.22 | 20 | 75.56 |
| 16 | 2.71 | 19.44 | 16 | 50.89 |
| **64 (default)** | **3.20** | **13.82** | **10** | **26.08** |
| 256 | 4.15 | 10.99 | 2 | 15.30 |

64 keeps the frames well inside what one palette (5.26) or full Spectrum 512
(7.14) reach while halving the error of the colours the room can show. 256
leaves almost nothing visibly wrong and still beats both.

The same pipeline on Monkey Island room 28, whose costumes 24, 26 and 37 the
room file owns, moves its 156 indices from 35.78 to 14.23 (18 to 9 visibly
off) and its 28 costume-only indices from 115.80 to 33.72 (10 to 6).

## Installing it

```sh
mkdir -p build-ste-scumm-static4/HD/SCUMMVM/MIX
cp devtools/atari-ste/monkey-bar/atlantis-room64-capture/flicker-compare-union/lut-dual16-dl0.20.bin \
   build-ste-scumm-static4/HD/SCUMMVM/MIX/DUAL20.BIN
```

`MIX\DUAL20.BIN` next to the program is the default table, so `SCUMMVM.INI`
needs no `ste_mix_lut` or `ste_mix_pattern`; `lastselectedgame=atlantis` starts
the game. Only one table can be installed, and it holds one room's colours: in
every other room the port maps each colour to the nearest pair the table's
palettes allow.

## A table per room

One table holds one room's colours, so the port now installs the table of the
room it is showing. `ScummEngine::startScene` tells the backend the room once
the room is set up and its palette is in place; `AtariGraphicsManager::
steSetRoom` looks for `R<room>.BIN` next to the default table, installs it and
redraws every buffer. A room without its own table falls back to the default
table, not to the table of the room before it, so the fallback is predictable.
Loading is a 1,920-byte read and one table rebuild, both far below the cost of
loading the room itself.

Tables for a whole game come from the game files, so no room has to be
captured:

```sh
node devtools/atari-ste/tools/scumm-ste-room-tables.mjs --game atlantis --all \
  --verb-bar 0,3,8,161,165,166,171,175,176,177,178,182,183,184,185,186,187 \
  --capture devtools/atari-ste/monkey-bar/atlantis-room64-capture \
  --out devtools/atari-ste/monkey-bar/atlantis-room-tables \
  --install build-ste-scumm-static4/HD/SCUMMVM/MIX
```

Per room it writes the colour list and fits the palettes to it
(`monkey-flicker-compare.mjs --room-colours`), then installs `R<room>.BIN`.
All **96 rooms of Fate of Atlantis build without a failure**; the set is
388 KiB on the drive and only one table is in memory at a time.
The palette is the room's own `CLUT`; where the engine overwrites entries, the
port falls back to nearest pairs for those entries alone. In Fate of Atlantis
room 64 the `CLUT` and the live palette are **identical in all 256 entries**,
and a table built without any capture scores 13.46 against the captured table's
12.41 over the 196 colours the room can show — the capture is worth having, but
not required. (Monkey Island is the counter-example: its engine sets entries 2,
3, 6 and 144 onward, so 67 of 256 entries differ from room 28's `CLUT`.)

The verb bar is drawn by the engine, so its palette entries are named once with
`--verb-bar`, measured from a capture in any room; 79 of the 96 Atlantis rooms
define exactly the same colours for them.

Reading every costume of a game found two faults in the host costume decoder,
both fixed against `BaseCostumeRenderer::byleRLEDecode`:

- A separate run-length byte of zero means **256**, because the engine
  decrements an 8-bit counter before testing it. The host decoder drew nothing
  and re-read, so a cel that used it never advanced and the tool hung; room 10
  of Fate of Atlantis is one such room.
- A cel header larger than the screen is a bad pointer, not art. Such cels are
  left out and counted (`rejectedCels`, one in room 10) instead of allocating
  their size.

All 240 Atlantis and 119 Monkey Island costumes decode after this. It changes a
few colour weights of the Monkey Island room-28 list, which is the fix taking
effect, not a regression.

## Where the cycles go

A Hatari instruction profile over 2,000 VBLs in room 64 (`profile on`, symbols
from the ELF, 320.5M cycles, ~48 frames), by symbol group:

| Group | Share |
|---|---:|
| Debug output through stdio (`fwrite`, `vfprintf`, …) | 21.8% |
| `convertMixRect` — the two fields, two table lookups per pixel | 19.2% |
| Software 32-bit multiply and divide (`__mulsi3`, `__udivsi3`, `__umodsi3`) | 18.3% |
| Chunky to planar (`asm_c2p1x1_4_rect`, `AtariSurface::copyRectToSurface`) | 13.0% |
| SCUMM drawing (costume Byle RLE, strips, `Gdi`, `testGfxUsageBit`) | 11.3% |
| Timer queue (`DefaultTimerManager::handler`, reinsertion) | 2.2% |
| VBL and Timer B handlers | 0.4% |
| Everything else (scripts, libc, TOS) | 13.7% |

Two things stand out. **The debug output is the single largest item**, because
`debuglevel=9` formats and writes every engine message; the staged
`SCUMMVM.INI` now sets `debuglevel=0`, which raises the idle bazaar from **41
to 30 VBLs per frame, 1.22 to 1.67 fps**. A flat profile cannot attribute the
arithmetic helpers to their callers, but the measured 1.37x also means part of
that 18.3% is `vfprintf` formatting numbers; the rest is the timer catch-up the
[room-28 measurement](STE_MONKEY_ROOM28_MEASUREMENT.md#timer-catch-up)
describes, plus address arithmetic.

What remains is the display path itself: ~19% converting and ~13% packing into
bitplanes, that is **a third of the frame for what the 68000 has to write
twice, once per field**. The engine's own drawing is 11%. Unlike Monkey
Island's room 28, strip decompression is not the leading cost here: room 64's
scripts redraw far less background.

## What was verified in Hatari

A 4 MiB STE, TOS 2.06, cycle-exact, sound off, started with
`C:\SCUMMVM\SCUMMVM.PRG`:

- The port logs `STE mix: MIX/DUAL20.BIN, two palettes, alternating, separate
  verb-bar palette` and no warning.
- With per-room tables installed it logs `STE mix: room 68 uses MIX/R068.BIN`
  and, when the game reaches the bazaar, `STE mix: room 64 uses MIX/R064.BIN`,
  and then maps all 256 colours from that table. The mouse still selects verbs
  and walks Indy after the change.
- With `R064.BIN` taken off the drive, the same run logs `STE mix: room 64 uses
  MIX/DUAL20.BIN`: a room without its own table falls back to the default one.
- In room 64 it logs `palette mapped, 256 colours from the table, 0 nearest`.
  Two screenshots one VBL apart hold the two fields; their linear-light average
  shows the bazaar in its VGA colours, as the evaluator's preview does. The
  fields themselves differ in 93% of the pixels, which is the flicker the
  average cancels.
- The mouse cursor is drawn: moving the mouse moves the crosshair, the old
  position is repainted, and the sentence line follows what it points at.
- Clicking works: a click on the `Open` verb selects it (`Open` on the sentence
  line, the verb highlighted), and pointing at the pots afterwards reads
  `Open pots`.

Mouse events were injected through Hatari's `--cmd-fifo` (`hatari-event
mousemove <dx> <dy>`, `leftdown`, `leftup`).

## The cursor

The STE converts the engine's finished 8-bit frame, so the backend draws the
cursor into that frame around the conversion (`Cursor::steUpdate/steDraw/
steRestore`) instead of blitting a cursor plane into the four-plane screen. Its
colours are palette indices, so the mixing table gives them pairs like any other
pixel and the cursor needs no reserved colours. Every buffer remembers the
rectangle its last conversion covered, so a cursor that moved is repainted in
all three of them. The engine's frame is unchanged outside the conversion, but
a capture that breaks inside it now sees the cursor.

## Open points

- **Speed.** 30 VBLs per converted frame in the idle bazaar, measured over
  320 conversions from the port's own counter: 1.7 complete frames per second,
  against the 5 VBLs a 10 fps frame would have. See below for where they go.
- **Rooms without a table** still fall back to nearest pairs of the default
  table, which can cost more than accuracy: the intro's temple turned purple
  and its darker half collapsed to black before room 68 had its own table.
  Building the whole set for a game is one batch run.
- **The costume list is a superset of what was seen, not a proof.** The room
  file's own costumes and the ones traced while the capture ran are what the
  union holds; an actor who only enters later, from another room's costume,
  is still missing. `--costumes` names such a costume by hand. Script analysis
  of `ENCD`/`LSCR` would settle it.
- **Sound is off** in every run above.
- **Real hardware.** The mixing has still only been judged in an emulator.
