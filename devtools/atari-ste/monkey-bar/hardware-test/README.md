# STE colour-mixing test kit: Monkey Island room 28

Everything needed to judge the two-field colour mixing on a real Atari STE,
apart from the game data. Hatari cannot answer these questions on a 60 Hz
host display: the two fields must alternate on a true 50 Hz screen.

- `SCUMMVM/SCUMMVM.PRG`: STE ScummVM build with colour mixing and a separate
  verb-bar palette, SHA-256
  `d3e5b465fbd3ee820b3e8ce9788e3c10d555a1eae3882a09bfd421b750a654c7`.
- `SCUMMVM/SCUMMVM.INI`: boots Monkey Island straight into the SCUMM Bar
  (`boot_param=28`) with `MIX\PAIR20.BIN` and the checkerboard pattern.
- `SCUMMVM/MIX/*.BIN`: mixing tables built from frames captured in room 28.

## Setup

1. Stock STE, 4 MiB RAM, TOS 2.06 (what Hatari verified), hard disk
   (the program is about 3 MB).
2. Copy the `SCUMMVM` folder to the root of drive C:.
3. Copy the DOS CD Monkey Island data into `C:\SCUMMVM\MONKEY`. The INI
   expects that path; adjust `path=` in `[monkey]` otherwise.
4. Use a 50 Hz PAL setup: a CRT or an RGB monitor at 50 Hz. The build keeps
   the STE at 50 Hz.
5. Run `SCUMMVM.PRG`. It starts in the bar. The game runs at a fraction of a
   frame per second in this room; that is expected.

## Switching tables

Edit `[scummvm]` in `SCUMMVM.INI` and restart:

- `ste_mix_lut=C:\SCUMMVM\MIX\PAIR20.BIN`: one palette. The number is the
  largest lightness difference between the two fields: `PAIR10`, `PAIR15`,
  `PAIR20`, `PAIR25`, `PAIR30` (0.10 to 0.30). Larger means better colour
  and more flicker.
- `DUAL10` to `DUAL30`: two palettes swapped per field. Better colour; the
  checkerboard cannot be used, so large areas flicker more.
- `ste_mix_pattern=checker` (default), `alternate` or `static`. DUAL tables
  always alternate.

ScummVM may rewrite `SCUMMVM.INI` on exit; the keys stay in `[scummvm]`.

## What to look for

Write down the table, pattern and viewing distance for each observation.

1. **Flicker on large areas.** Walls, tables, the floor. Start with `PAIR10`
   and go up to `PAIR30`. Which is the largest limit you would accept at a
   normal viewing distance? Does flicker show more in peripheral vision?
2. **Checkerboard against alternation.** With `PAIR20`, compare
   `ste_mix_pattern=checker` and `alternate`. Alternation should flicker
   visibly more on large areas; the checkerboard should look calmer.
3. **Fine shimmer.** Close up, does the checkerboard read as a 25 Hz
   shimmer or crawl, especially around Guybrush and the pirates?
4. **Two palettes.** `DUAL20` and `DUAL30`: is the better colour worth the
   stronger area flicker?
5. **Verb-bar boundary.** Screen lines 144 and 145 (the black line above the
   "Walk to" sentence, and the sentence text). A Timer B interrupt switches to
   the verb-bar palette there. Look for flashes of wrong colours on the
   sentence line, especially while moving the mouse, pressing keys or during
   disk access.
6. **Whole-screen colour flashes.** A missed VBL shows one field twice. Watch
   during room loading and disk access.
7. **Colour and detail.** Guybrush's white shirt, orange hair and face; the
   green bottle of the pirate on the chandelier; the blue windows.
8. **Stability.** Crashes, hangs, or garbage on screen.

Hatari checks of this build: both fields match the tables pixel for pixel,
the verb-bar palette switches exactly at line 144, and all 256 room colours
come from the table.
