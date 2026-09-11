# Atari STE port tools

Host tools, tests and notes for the game-only Atari STE backend
(`backends/graphics/atari/atari-ste-*`, `backends/platform/atari/build-ste-scumm.sh`).
They target Monkey Island's SCUMM Bar (room 28) on a stock 8 MHz STE with
4 MiB RAM, measured in cycle-exact Hatari. The tools are Node.js scripts
without npm dependencies; run them from the repository root.

## Layout

- `tools/`: room-28 extraction and costume decoding, palette-strategy
  comparisons, the native viewer build, frame capture and per-phase profiling
  of the running port, and `monkey-flicker-compare.mjs`, which writes the
  `ste_mix_lut` colour-mixing tables.
- `raster-test/`: a standalone program for the Timer B palette raster;
  `tools/timerb-test.mjs` builds, runs and verifies it.
- `raster.s`: the Spectrum 512 raster loop from Spectrum512Painter, included by
  the native room viewer `monkey-bar/monkey-viewer.s`.
- `tests/scene-renderer/`: host (ASan/UBSan) and 68000 tests of the
  unmodified scene renderer, driven by `tools/scumm-ste-rect-test.mjs`.
- `docs/`: design notes and measurements. `STE_MONKEY_TEMPORAL_MIXING.md`
  describes the current display path; `STE_MONKEY_ROOM28_MEASUREMENT.md` the
  CPU budget.
- `monkey-bar/`: generated output. Only its README, `monkey-viewer.s` and
  `hardware-test/README.md` are tracked; extracted fixtures, captures and
  tables derive from the game and stay out of git.

## Paths

The defaults expect these checkouts next to this repository; environment
variables override them.

| Variable | Default |
|---|---|
| `STE_BUILD` | `build-ste-scumm-static4` in this checkout |
| `HATARI`, `TOS`, `VASM` | `../F030Arcade/third_party/…` |
| `NM`, `CXXFILT`, `MINT_BIN` | `../cross-mint/bin` |
| `MONKEY_DATA_DIR` | `assets/monkey-cd` in this checkout, else `../scummvm/assets/monkey-cd` |
| `SCUMM_STE` | this checkout (renderer sources for the rectangle test) |
| `HD` | `build-ste-scumm-static4/HD` (for `scumm-ste-capture.mjs`) |

Game data is never committed; keep it in the untracked `assets/` directory.

## Common tasks

```sh
node devtools/atari-ste/tools/scumm-ste-frame-capture.mjs --out devtools/atari-ste/monkey-bar/room28-capture
node devtools/atari-ste/tools/scumm-ste-frame-capture.mjs --game atlantis --boot-param 9554 --room 64 --vbls 44000,44400,44800,45200,45600,46000,46400,46800 --out devtools/atari-ste/monkey-bar/atlantis-room64-capture
node devtools/atari-ste/tools/monkey-flicker-compare.mjs --capture devtools/atari-ste/monkey-bar/room28-capture --split 144 --dl 0.1,0.15,0.2,0.25,0.3 --sheet-dl 0.2 --out devtools/atari-ste/monkey-bar/room28-capture/flicker-compare-split
node devtools/atari-ste/tools/scumm-ste-room-profile.mjs --first 8 --last 40 --out devtools/atari-ste/monkey-bar/room-profile/walk-rerun
node devtools/atari-ste/tools/timerb-test.mjs /tmp/timerb
node devtools/atari-ste/tools/scumm-ste-rect-test.mjs
```

`monkey-bar/README.md` covers fixture extraction, the native viewer and the
colour-mixing pipeline; `monkey-bar/hardware-test/README.md` is the checklist
for judging the mixing on a real STE.

## History

The tools and notes were developed in the Spectrum512Painter repository and
moved here on 2026-09-11. References to the moved files are updated.
`ste/raster.s` and the Spectrum 512 ball demos the notes mention remain in
Spectrum512Painter, and the handoff notes keep their original machine paths.
