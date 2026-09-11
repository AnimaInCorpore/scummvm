# Developer handoff: Monkey Island DOS CD on STE

Prepared 2026-09-10. Continue implementation from the agreed benchmark; the
research and scene selection are complete. Read
[STE_MONKEY_SCUMM_BAR_BENCHMARK.md](STE_MONKEY_SCUMM_BAR_BENCHMARK.md) for the
complete fixture specification and measured-source facts.

## Objective and accepted decisions

Build an optimizing test harness around the crowded **SCUMM Bar, room 28**, in
Monkey Island DOS CD, Part One. Target a stock **8 MHz 68000 Atari STE, 4 MiB
RAM, 4096-color gamut**. This effort supports this one game/version, rather than
a general DOS renderer.

The user's priority is **fidelity first, then speed at 10 complete scene updates
per second**. Preserve detail and stable colors during motion. Keep display
refresh at the independently verified 50/60 Hz raster rate. The eventual 10 fps
gameplay result includes engine and audio cost; report renderer-only results
separately. The memory limit covers the whole program, resources, buffers,
tables, and audio, not just the graphics component.

There is **no required background/actor palette split**. The earlier 10+6 split
was a speculative ball-demo experiment. Compare other allocations, shared
palettes, and alternative raster schedules. Room-specific precomputation and
specialized drawing code are encouraged where they improve fidelity within the
runtime limits. Host-side optimization can be expensive.

The room data is 640x144. Display a 320x144 scene viewport with the original
bottom interface in the remaining 56 rows, confirming live virtual-screen
boundaries during capture. Restrict the expensive raster region to the scene
while the interface is present. Full-screen scenes will need their own budget.

## Protect the parallel Falcon work

The Falcon port is active in `/Users/saschaspringer/Work/scummvm` under another
developer. All inspection in this task has been read-only.

- Keep the STE harness and generated experiments in Spectrum512Painter.
- Do not switch branches, reset, rebuild, edit, or drive the active Falcon
  checkout or its running emulator.
- If SCUMM instrumentation is needed, use a separate checkout/worktree with a
  separate build directory and output paths. Inspect and follow that project's
  applicable instructions. Select its starting revision deliberately; a
  worktree from HEAD does not include the active checkout's uncommitted work.
- Read original game assets from the sibling path without changing them.

Spectrum512Painter itself contains substantial modified and untracked ball-demo
work predating this task. In particular, `ste/README.md`, `ste/raster.s`, and
`devtools/atari-ste/tools/reference.mjs` were already modified, with many untracked native
sources, assets, binaries, and verification directories. Preserve all of them.
Prefer separate names/directories for the Monkey Island experiment. Do not
assume an isolated worktree from HEAD contains those untracked experiments.

Follow `AGENTS.md`: modular ES modules, no new third-party dependencies or CDNs,
self-contained browser implementation, and GEM-style English controls for any
new browser UI. Use the existing `ste/` experiment structure for native STE
measurements.

## What has actually been completed

- Read the ball-demo documentation, raster code, conversion helpers, and relevant
  local SCUMM palette/screen/backend code.
- Inspected the existing Falcon screenshot of the crowded bar.
- Read and decoded resource container metadata from MONKEY.000/MONKEY.001.
  Room 28 has a 640x144 RMHD, a 256-color CLUT, and 52 object records. Object
  records are not a count of simultaneous actors. Exact hashes and offsets are
  in the benchmark specification.
- Selected the fixture and documented alternatives, scrolling constraints,
  quality priorities, and the 10 fps target.
- Added a repeatable room-28 extractor, indexed background/object previews, and
  source manifests under `devtools/atari-ste/monkey-bar/fixture/`.
- Added a native STE scene renderer that decodes real DOS-CD V5 `COST`
  resources 24, 26, and 37, composites their world-space Byle-RLE cels as one
  ordered scene frame, and converts that completed 320x144 viewport with the
  joint 16-register Spectrum schedule.
- Built and tested the renderer with the F030Arcade Hatari, including the
  144-row scene / 56-row GEM panel split and a 31-frame deterministic animation
  sequence. The current scene is a renderer milestone, not full SCUMM script,
  collision, audio, or camera integration.

The remaining engine-level work is to obtain exact actor state, occlusion,
conversation timing, scrolling, and audio from a complete SCUMM execution. The
native scene currently uses calibrated room-world anchors from the room-28
reference and is intentionally bounded to the selected sprite frame set.

The principal files added for this implementation are:

- `devtools/atari-ste/docs/STE_MONKEY_SCUMM_BAR_BENCHMARK.md`
- `devtools/atari-ste/docs/STE_MONKEY_HANDOFF.md`
- `devtools/atari-ste/tools/monkey-costume.mjs`
- `devtools/atari-ste/tools/monkey-scene.mjs`
- `devtools/atari-ste/tools/generate-monkey-native.mjs`
- `devtools/atari-ste/monkey-bar/monkey-viewer.s`

## Implementation order

### 1. Extract and verify the real fixture

Start here. Implement a repeatable, bounded parser/extractor using existing
project facilities and standard runtime APIs. Parse container offsets rather
than relying on the observed room offset as a permanent format rule. Preserve
original palette indices and palette events. Decode the 640x144 indexed room
background, relevant object images, and foreground masks; obtain actual actor
costumes/poses through the appropriate resources or isolated engine capture.

Produce a source preview and a manifest identifying the game data, room,
dimensions, and extracted data. The existing RGB PNG reader in `devtools/atari-ste/tools/png.mjs`
does not support indexed PNG input; do not assume it preserves source indices.

Then obtain a deterministic sequence containing ambient animation, walking,
occlusion, conversation, and actual camera movement. Export timestamps, camera
offsets, screen boundaries, indexed pixels/palettes, and dirty regions. Obtain
layer/mask/draw-order information where evaluating specialized actor drawing.
Flattened frames suffice for output comparison but not for proving actor
composition cost. Do not substitute moving cutouts of a screenshot for actual
game animation and call that a game benchmark.

### 2. Establish fidelity references and compare techniques

Build a modular browser preview/comparison harness using the original source
sequence. Reuse the existing conversion code where appropriate. Explicitly
select `bitsPerColor: 4`; the UI's 32768-color enhanced target is outside scope.

Compare shared palettes, multiple fixed reservations, joint Spectrum palettes,
and allocations varying by room region. Include a palette-per-line/band path
for scrolling and raster-cost comparison. Quantify scene and actor errors
separately and inspect motion, text, silhouettes, and stationary background
stability. Compare no dithering with spatial dithering where useful; the user
has not mandated either. Measure visual quality against the original palette,
and distinguish gamut quantization loss from renderer palette-allocation loss.

First deliverable: a reproducible bar fixture/sequence and a visual comparison
of at least the shared line-palette and joint Spectrum candidates, accompanied
by quality metrics and prepared-data sizes. This is a real implementation
milestone; another design-only document does not complete it.

### 3. Implement the selected native display and redraw path

Use a separate Monkey Island experiment under `ste/`. Present bitmap and
matching palette state atomically, retaining each buffer's restoration history.
Implement the 144-row scene/interface split and verify row-zero synchronization
instead of blindly truncating the existing full-height raster loop.

Reduce runtime cost with precomputed mappings, bounded caches, planar drawing,
dirty regions, and specialized routines justified by measurements. Changing a
palette can affect background pixels outside a sprite's dirty rectangle; account
for every affected pixel. Keep a correct fallback for uncaptured game states.

Measure the chosen routine with raster display enabled. The existing raster
waits inside the VBL routine and masks interrupts; shortening its active region
alone does not automatically recover all border time. Scheduling, input, DMA
audio, and storage interactions need verification in the native implementation.

### 4. Add and measure hardware scrolling

The same 640-pixel room provides the camera fixture. Use coarse address changes,
fine-scroll phases, and the correct stride; restore a fixed interface address
and scrolling state at the split.

Hardware scrolling moves pixel indices while Spectrum writes follow beam
positions. A shifted bitmap with an unchanged palette schedule can change
colors. Compare a horizontally stable line-palette baseline against a
scrolling-aware Spectrum schedule plus all necessary corrections/cache changes.
Sixteen fine-scroll phases do not cover all coarse camera positions by
themselves. Check actual camera offsets and a separately labelled 0..320 stress
sweep, including 15->16 and 16->15 transitions.

### 5. Validate the game budget

Target 10 complete updates per second on cycle-exact 8 MHz STE emulation,
then verify on real hardware when available. Measure CPU cycles, raster
occupancy, update-time spikes, and total resident memory. The preliminary
512 KiB table allowance is adjustable if the complete 4 MiB budget permits.

Include engine execution and streamed audio before declaring the gameplay
target met. A native sequence player is an intermediate renderer benchmark.
The bar's CYCL payload starts with a zero terminator; add a verified second
fixture if needed to cover actual palette cycling, and keep original palette
index identities separate even where current RGB values coincide.

## Useful code and tooling

Paths below are relative to Spectrum512Painter unless marked otherwise:

- `js/imaging/spectrum512.js`: existing converter and options.
- `js/imaging/spectrum512-bruteforce.js`: host-side refinement.
- `js/imaging/spectrum512-slots.js`: standard schedule's position-dependent
  slot mapping. Custom schedules require their own matching model.
- `devtools/atari-ste/tools/reference.mjs`: fixed-point color metric, RGB12/STE encoding,
  planar line conversion. It is already modified; avoid overwriting it.
- `ste/raster.s`, `ste/limited-raster.s`: existing 60 Hz timed loops.
- `ste/fullcolor-render.s`, `ste/tools/fullcolor-palettes.mjs`: palette-family
  experiment and affected-background-row handling; not a proven game solution.
- `ste/tools/verify-colored.mjs`: example independent compositor, symbol
  extraction, exact buffer comparisons, frame cadence, and desktop cleanup.
- `ste/tools/emulate.mjs`: existing Hatari capture/profiling approach.

The existing scripts default to these tool paths, with environment overrides:

- `VASM`: `/Users/saschaspringer/Work/F030Arcade/third_party/vasm/vasmm68k_mot`
- `HATARI`: `/Users/saschaspringer/Work/F030Arcade/third_party/hatari/build/src/hatari`
- `TOS`: `/Users/saschaspringer/Work/F030Arcade/third_party/tos/tos206de.img`

Check availability before use. Existing generator/verifier commands can rewrite
their own assets and binaries; adapt their patterns into separate output paths
for this experiment. Some historical verification runs use `--sound off`.
Also, `RAW=1` in `emulate.mjs` disables the raster handler for isolated profiling;
that mode cannot establish displayed-game throughput.

Read-only SCUMM references in the sibling repository:

- `engines/scumm/gfx.cpp`: `initScreens`, dirty strips, and screen submission.
- `engines/scumm/palette.cpp`: palette updates and cycling.
- `engines/scumm/resource.cpp`, `engines/scumm/room.cpp`: resource loading.
- `backends/platform/atari/osystem_atari.cpp`: existing backend explicitly
  requires TT/Falcon video; STE support is a separate implementation task.

ScummST is a useful 8 MHz game-engine precedent, with converted assets and
ordinary 16-color output: <https://github.com/agranlund/ScummST>. Its existence
does not establish the additional Spectrum raster budget.

## Handoff prompt

Continue the STE Monkey Island DOS CD work in Spectrum512Painter. Read
AGENTS.md, devtools/atari-ste/docs/STE_MONKEY_HANDOFF.md, and
devtools/atari-ste/docs/STE_MONKEY_SCUMM_BAR_BENCHMARK.md. Implement the SCUMM Bar fixture and
optimizing test harness, starting with indexed extraction and deterministic
animation capture. Prioritize fidelity, then 10 complete game updates per
second on an 8 MHz STE with 4 MiB RAM and 4096 colors. Treat palette allocation
and raster technique as search variables. Preserve the active Falcon checkout
and all pre-existing ball-demo work. Use an isolated SCUMM checkout if capture
instrumentation is necessary. Carry the first implementation milestone through
verification and report clearly what is a host preview, a native renderer
measurement, and an integrated game measurement.
