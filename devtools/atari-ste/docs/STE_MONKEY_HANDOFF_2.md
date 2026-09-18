# Developer handoff 2: STE fidelity strategy for Monkey Island / Fate of Atlantis

Continues [STE_MONKEY_HANDOFF.md](STE_MONKEY_HANDOFF.md) and
[STE_MONKEY_SCUMM_BAR_BENCHMARK.md](STE_MONKEY_SCUMM_BAR_BENCHMARK.md). Read those
first for the fixture spec and measured-source facts. This document records what
the fidelity-strategy session established, the two blockers that gate the port,
and the concrete next steps.

## Goal (unchanged)

An Atari STE ScummVM port of the DOS-CD games **Monkey Island** and **Fate of
Atlantis** that stays close to VGA fidelity while running **~10 complete scene
updates/second** on a stock 8 MHz / 4 MiB STE, 4096-colour gamut. Fidelity
first, then speed. Display refresh stays at the verified 50/60 Hz raster rate.

## Repositories and their roles

- `Spectrum512Painter/ste/` — host tooling, experiments, and assets. Work here.
- `/Users/saschaspringer/Work/scummvm-ste-scene` — the STE ScummVM checkout
  (separate worktree, detached HEAD). Build with
  `backends/platform/atari/build-ste-scumm.sh`; staged HD image at
  `build-ste-scumm-static4/HD/SCUMMVM/SCUMMVM.PRG`. Cross-mint toolchain:
  `/Users/saschaspringer/Work/cross-mint/bin`.
- `/Users/saschaspringer/Work/scummvm-ste-capture` — host SDL build with room-28
  capture instrumentation (`ste_capture_room` / `ste_capture_dir`, ini
  `ste-capture.ini`). Restored to its as-found state at end of session.
- `/Users/saschaspringer/Work/scummvm` — **active Falcon port, read-only.** Do
  not switch/reset/rebuild/drive it. Original DOS-CD data is under
  `assets/monkey-cd` (read-only).

Tool env overrides (defaults point into the sibling F030Arcade project):
`VASM=…/vasm/vasmm68k_mot`, `HATARI=…/hatari/build/src/hatari`,
`TOS=…/tos/tos206de.img`.

## What this session established

### The real room-28 actor set is costumes 24, 26, 37

The costume directory (`DCOS`, `room == 28`) lists exactly costumes **24, 26,
37** — the set `devtools/atari-ste/tools/monkey-scene.mjs` already used. The costume *set* was
correct; only the world positions were hand-calibrated. Real placement now comes
from the room's walkboxes (`BOXD`): actor feet occupy screen rows **109–140**
(ignoring the sentinel gate box at −32000). Derived on-screen bands:

- COST 24 — fixed hanging object, rows 31–89.
- COST 26 — floor walker, rows 61–140.
- COST 37 — floor walker, rows 62–123.

### Fidelity ladder (real SCUMM Bar, mean Oklab err/px, lower = better)

Measured by `devtools/atari-ste/tools/monkey-strategy-compare.mjs` (single frame) and
`devtools/atari-ste/tools/monkey-actor-schedule.mjs` (across 31 motion frames):

| strategy | writes/line | err/px |
|---|---|---|
| single 16-colour palette / frame | 0 | 14 |
| per-line 16 fit to one frame | 1 | 4 |
| per-line 16 frozen over full motion | 1 | 13.5 |
| reserved 10 bg + 6 sprite (EIGHT.PRG split) | 1 | 13 |
| full Spectrum 512, 48 colours/line | 3 | ~1–2 |

### The two conclusions

1. **Freeze a union-aware schedule and motion stays stable.** Choosing each
   scanline's palette over the *union* of every colour that can appear there (all
   costumes, poses, positions) and freezing it means a moving actor never
   recolours scenery. Verified: **0 of 42,439** always-background pixels
   recoloured across 31 frames.
2. **16 colours/line cannot hold VGA fidelity here.** Room 28's per-line colour
   demand runs 11–60 and exceeds 16 on **134/144** lines, so a frozen 16-colour
   schedule degrades to 13.5 err/px under motion. **Full Spectrum 512 (48/line,
   3 beam-timed writes) is required for fidelity-first**; the existing
   frame-sampled Spectrum schedule (`chooseStablePaletteRows` in
   `generate-monkey-native.mjs`) already sits near 2 err/px.

New host tooling (no new deps): `devtools/atari-ste/tools/monkey-actor-schedule.mjs`,
`devtools/atari-ste/tools/monkey-strategy-compare.mjs`, `devtools/atari-ste/tools/palette-select.mjs`. Outputs
in `devtools/atari-ste/monkey-bar/actor-schedule/` and `devtools/atari-ste/monkey-bar/strategy-compare/`.

## Blockers (these gate everything)

### 1. The per-scanline palette raster is disabled — the display gate

In `scummvm-ste-scene/backends/graphics/atari/atari-graphics.cpp` the VBL body
has `if (false && s_ste && atari_ste_display_palette) atari_ste_raster_body();`,
and `atari-ste-raster.S` only writes 16 palette words once. So the hardware
shows **one palette for the whole 200-line frame** — the per-line palettes that
`AtariSteSceneRenderer` computes never reach the screen, which is the banding you
see. No palette strategy is visible until a real per-scanline raster exists.

The standalone `ste/raster.s` proves the Spectrum beam-timed technique but it
busy-waits the whole frame in the VBL, which cannot coexist with the engine.
**Next: install the schedule from a Timer-B interrupt** (fires per display line),
freeing the CPU for the engine. No Timer-B/HBL infrastructure exists in the
backend yet; the only interrupt hook is the 200 Hz timer in
`osystem_atari.cpp`.

### 2. Runtime quantisation cannot hit 10 fps

`AtariSteSceneRenderer::convert()` runs a per-pixel Oklab colour search in C on
the 68000; the prior session measured ~3 s/frame (~30× off target). The offline
demos (EIGHT.PRG, FULLBALL.PRG) reach interactive rates precisely because they do
**zero runtime colour search**: all quantisation and palette selection happen on
the host at asset-build time; the runtime only blits precompiled planar rows and
swaps precomputed palette words. The port must adopt this.

### Capture limitation

Live room-28 engine capture was not obtainable headless in this environment: the
instrumented build (and the uninstrumented `scummvm/build-full/scummvm`) stall in
boot under `SDL_VIDEODRIVER=dummy`. The real actor set was therefore derived
statically from resources (`DCOS` + `BOXD`), which is a safe superset for "all
possible colours." A dev with a real display should capture actual walk tracks to
tighten the actor bands and to confirm which other actors enter the room.

## Next steps, in order

1. **Wire a per-scanline palette raster (Timer-B) into `scummvm-ste-scene`.**
   Install the precomputed schedule per display line without blocking the engine.
   This unblocks all fidelity work. Validate first with a per-line 16-colour
   schedule (simplest), then extend to Spectrum (3 writes/line).
2. **Move quantisation to host/asset-build time.** Hook room loading to the
   `ste/tools` pipeline: prerender the planar background + its frozen per-scanline
   schedule, and precompile planar actor rows. Replace `convert()`'s runtime
   search with: blit ~2400 dirty actor pixels over the planar background + install
   the schedule.
3. **Extend the union-aware solver to the 48-slot Spectrum schedule.**
   `monkey-actor-schedule.mjs` currently solves per-line 16; add the 3-zone
   Spectrum model. Caveat: Spectrum's per-line colour freedom relies on
   horizontal locality, which an actor walking across a zone boundary can break
   (see the benchmark's scrolling/colour section) — account for every affected
   pixel.
4. **Generalise beyond room 28.** Repeat the DCOS/BOXD actor-set + band
   derivation per room; bound cached per-room schedules and keep a correct
   fallback for states absent from the captured/derived sample.
5. **Measure the real game budget** (engine + audio + raster) at 10 fps in
   cycle-exact Hatari, then on real hardware.

## Reproduce this session's results

From the repository root:

```sh
node devtools/atari-ste/tools/monkey-strategy-compare.mjs --frame 0
node devtools/atari-ste/tools/monkey-actor-schedule.mjs
```

Build + run the STE binary in the sanctioned Hatari (kill any existing Hatari
first):

```sh
/Users/saschaspringer/Work/F030Arcade/third_party/hatari/build/src/hatari \
  --tos /Users/saschaspringer/Work/F030Arcade/third_party/tos/tos206de.img \
  --harddrive /Users/saschaspringer/Work/scummvm-ste-scene/build-ste-scumm-static4/HD \
  --machine ste --memsize 4 --cpulevel 0 --cpuclock 8 \
  --cpu-exact on --compatible on --sound off --fast-boot on --confirm-quit off \
  --frameskips 0 --borders off --statusbar off --zoom 2 \
  --conout 2 --auto 'C:\SCUMMVM\SCUMMVM.PRG'
```

Resident heap after `resetScummVars` is ~360 KiB free behind the ~2.9 MiB image,
so precomputed tables must fit that budget (the benchmark's 512 KiB table
allowance is the planning ceiling).

## Update — per-scanline raster wired and verified (next-steps #1 done)

Next-steps item 1 (wire a per-scanline palette raster via Timer B into
`scummvm-ste-scene`) is implemented and verified. The LucasFilm Games intro of
Monkey Island now renders on a stock 8 MHz STE in cycle-exact Hatari with a
different 16-colour palette on every scanline (≈45 colours across the frame),
no crash, and full-speed boot.

### What was built

- **`backends/graphics/atari/atari-ste-raster.S` (rewritten).** Two-part
  beam-timed raster. A VBL vector hook ($70, independent of the TOS VBL queue
  and of `vblsem`) presents the pending screen/schedule pair, writes lines
  0/1's palette, and arms MFP Timer B to fire at the end of line 0. The Timer B
  handler re-synchronises to line 1 through the STE video counter (killing
  interrupt jitter) and then runs the proven `ste/raster.s` per-line write loop
  for the scene lines with interrupts masked, then returns — so the engine keeps
  the CPU outside the scene band. `getMillis()` adds back the 200 Hz ticks the
  masked window swallows (`atari_ste_raster_lost_ticks`).
- **Schedule packing** in `atari-ste-scene.cpp`: the per-line 16-colour
  schedule the renderer already chose is packed into the stream the raster
  plays (`atari-ste-raster.h` documents the layout) once per schedule change,
  tracked per buffer via `stePaletteGeneration`.
- **Presentation handshake + gating** in `atari-graphics.cpp`: the STE presents
  from the raster's VBL hook (works while the engine holds the TOS VBL lock).
  The raster stays idle until the scene renderer has produced a real schedule
  for a hidden-overlay frame, so boot/loader/overlay run at full engine speed.
- **Config**: `ste_raster` (default true) master-enables the Timer B loop;
  `ste_raster_lines` (144) and `ste_raster_60hz` (false) as before. With
  `ste_raster=false` the frame keeps one stable palette (no per-line raster).

### The one bug that mattered

The Timer B sync originally accepted the beam up to 150 bytes into the line, so
the variable jump into the 150-byte NOP delay sled overshot and started
executing mid-write with a stale pointer — scribbling palette words into RAM
(uniform colour standalone; an odd-address bus error in the full engine).
Fix: only lock when within 12 bytes of the line start, else advance to the next
line. One line of asm.

### Verification harness (in `Spectrum512Painter/ste`)

- **`timerb-raster-test.s`** — standalone STE program that installs the *same*
  raster and plays a 200-line schedule with a distinct colour per line.
  **`tools/timerb-test.mjs`** builds it, runs it in Hatari, dumps the raster
  counters, and checks every displayed row against the schedule:
  **0 of 200 rows mismatch** — the per-line palette is pixel-exact. Fast
  (one short boot); use it to iterate on any raster-timing change.
  **`tools/timerb-schedule.mjs`** generates its schedule.
- **`tools/scumm-ste-capture.mjs` / `scumm-ste-verify.mjs`** — capture a frame
  from the staged `SCUMMVM.PRG` in Hatari (screenshot + screen/schedule dump)
  and rebuild the expected frame from the dump for a byte-level check. (The
  counter/dump reads rely on Hatari resolving the ELF's symbols; the screenshot
  path works regardless and is what confirmed the logo above.)

### Known minor items / next

- **1-scanline top offset**: the raster's first loop iteration lands on scene
  line 3, so scene line 2 keeps the header (line-0) palette. Cosmetic; tighten
  if it shows.
- Still **16 colours/line**, not the 48-slot Spectrum. This unblocks the
  display path; next-steps #3 (extend the union-aware solver to the 3-write
  Spectrum schedule) is the fidelity step and slots into the same packed-stream
  contract (`atari-ste-raster.h`), which already reserves 48 words/line.
- Reaching **room 28** headless still needs a save or scripted input; the logo
  is the intro the engine renders on its own. Budget/occupancy at 10 fps
  (next-steps #5) is now measurable with the raster actually running.
