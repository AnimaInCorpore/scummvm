# STE decision schedules: first measurements, 2026-09-11

Follow-up: [actual room-28 engine and renderer measurement](STE_MONKEY_ROOM28_MEASUREMENT.md)
now establishes the walking/idle frame budget. The isolated timings below
remain a microbenchmark, not the complete room cost.

Precomputing the colour decision remains a useful direction. The first runtime
change and the missing 32-colour comparison are now implemented. The proposed
cycle and table budgets are not yet established; phase 1 does not pass its gate.

## Changes and validation

In `scummvm-ste-scene/backends/graphics/atari/`, `updateScreenInternal()` now
passes the destination buffer's existing dirty-rectangle set directly to
`AtariSteSceneRenderer::convert()`. `convertRect()` clips the rectangles, aligns
x to 16 pixels, quantizes their horizontal extents, and submits those spans to
the existing c2p routine. Full redraw and palette invalidation still rebuild
and reconvert the full frame. Smaller source surfaces are centered with cleared
padding, without reading beyond their rows. There is no new persistent buffer.
Overlapping rectangles may convert their intersection twice; this change does
not introduce a region-union allocator.

The complete static STE executable builds successfully:
`scummvm-ste-scene/build-ste-scumm-static4/scummvm.prg`. It has not replaced the
staged HD executable. An isolated copy reaches the Lucasfilm intro. At VBL
6000, `scumm-ste-verify.mjs` reports 0 mismatches across all 64,000 pixels when
reconstructing the screenshot from the dumped planar screen and packed
schedule (45 distinct start-of-line colours). Artifacts:
`devtools/atari-ste/monkey-bar/rect-runtime/verified/`. This is a single-frame check; its
counters also show 2,112 raster resyncs and 9 skipped raster frames, so it is
not a claim of flawless raster timing. The largest free block after
`resetScummVars` is 347 KiB in this run.

The capture harness now quotes evaluated `savebin` addresses, closes unused
debugger stdin, and disables the emulator's drive-LED overlay. Those fixes
allow the dump comparison to run and prevent the LED from appearing as a
false pixel mismatch.

`devtools/atari-ste/tools/scumm-ste-rect-test.mjs` compiles an unmodified copy of the actual
scene renderer against small surface adapters. The host run uses ASan/UBSan
and a scalar planar encoder. The Hatari run links the production 68000 c2p
assembly and uses NF_CYCLES. Both pass output comparisons covering aligned and
unaligned rectangles, clipping, empty rectangles, padded source pitches,
304/319/320-wide sources, overlapping dirty histories, stale per-buffer schedule
generations, and rectangles crossing into the verb panel. Partial output is
byte-identical to a full conversion using the same frozen schedule.

## Phase 1 timing: gate not met

Cycle-exact Hatari, STE, 4 MiB, 68000, 8 MHz, RGB monitor, TOS 2.06 DE;
configuration isolated from the user's saved Hatari settings. A synthetic
48x40 rectangle is measured after warming the lazy slot maps:

| Conversion | Submitted pixels | Elapsed CPU clocks |
|---|---:|---:|
| aligned rectangle | 1,920 | 796,488 |
| full-width dirty rows | 12,800 | 3,896,860 |

This is a 6.67x reduction in submitted pixels and a 4.89x cycle improvement.
It is still almost 8x above the 100,000-cycle phase-1 target. These numbers
include the converter's per-call palette preparation and ordinary TOS activity;
they exclude SCUMM, audio, and the Spectrum raster. The surface adapters differ
from ScummVM's complete graphics backend. This is a renderer microbenchmark,
not a measured walking-actor game frame or a phase-6 result.

Raw output: `devtools/atari-ste/monkey-bar/rect-test-hatari/hatari.log`.

The 10–12 cycles/pixel fused-converter assumption also needs revision. On a
68000, a straightforward source-byte read followed by a register-indexed
map-byte read already costs approximately 8 + 14 cycles, before planar packing,
stores, loop control, or bus stalls. A different packed lookup design would
need its own measured budget.

## Missing 32-colour comparison

The comparison now evaluates both ways to retain one visible Spectrum reload:
the early register transitions, or the late transitions. Both also require the
border group that initializes the following line. The model preserves the
actual staggered register positions; it does not assume a simultaneous switch
of all 16 registers. The existing 48-word conversion remains the reference.

Mean fixed-point Oklab squared error/pixel against the RGB12 source:

| Single-frame fit | Frame 0 | Frame 15 | Frame 30 |
|---|---:|---:|---:|
| per-line 16 | 3.704 | 4.147 | 3.704 |
| 32, early visible reload | 2.994 | 3.341 | 2.994 |
| 32, late visible reload | 1.624 | 1.924 | 1.624 |
| 48, both visible reloads | 0.868 | 1.099 | 0.868 |

Outputs and contact sheets: `devtools/atari-ste/monkey-bar/strategy-32/frame-{0,15,30}/`.
`verify-monkey-spectrum-model.mjs` checks all register transition boundaries
and reconstructs RGB from the emitted planar pixels and slot schedule.

These are independently fitted frames, not one schedule frozen over motion.
The late-reload 32-colour candidate warrants the next frozen-schedule experiment;
these results do not yet pass the motion-fidelity gate.

The existing room-union model has demand above 16 on 134/144 lines, above 32 on
100/144, and above 48 on 42/144. Its frozen 16-colour result remains 13.51
error/pixel. The model covers the room background and selected animations of
COST 24, 26, 37; it does not establish coverage of every actor entering the room,
every animation, script palette state, subtitle, or cursor. Its stability check
counts positions whose source colour stays identical across sampled frames;
that alone does not prove that all counted positions are background.

## Corrections required before defining the room blob

1. **Three simple zone maps are insufficient for arbitrary Spectrum palettes.**
   Register `r` first changes at `10*r + (r odd ? -5 : 1)` and changes again
   160 pixels later. This creates 33 active-palette intervals across a row,
   with mixed old/new registers during transitions. A naive byte map for every
   interval is `33*144*256 = 1,216,512` bytes. That is not a proposed format;
   it demonstrates why the 110,592-byte three-map estimate needs a constrained
   assignment or compression scheme, validated against the x/register model.

2. **Fewer writes do not automatically recover engine cycles.** The current
   Timer-B handler masks interrupts and owns the entire raster band. Removing
   one write group and replacing it with timing padding leaves that occupancy
   intact. A two-write or adaptive design must actually return useful execution
   windows to the engine and prove resynchronization. Its smaller packed stream
   saves storage, but the present player always consumes 48 words per line.

3. **Room-wide freezing needs an explicit coverage contract.** Resource
   ownership in DCOS and sampled animations are not an exhaustive list of room
   participants or palette mutations. Scrolling may also change reconstructed
   background colours if zone assignment changes; zero recolouring during a
   fixed-camera walk does not establish zero recolouring during camera motion.

4. **Cycling and fades need CLUT identity, not just RGB values.** The current
   schedule stores selected RGB12 values. Patching an indexed cycle requires
   retaining which CLUT entries own each reserved slot in every relevant group.
   Fade emission can preserve those assignments, but needs a defined base
   palette and rounding contract. The existing runtime RGB12 conversion uses
   `((channel+8)>>4)&15`, which wraps 248..255 to zero and differs from the host's
   clamped `round(channel/17)`. Resolve that before claiming host/runtime byte
   identity for room assets.

5. **Count the current buffers.** At 144 lines one packed schedule occupies
   `2*(32+48*142) = 13,696` bytes. `allocateSurfaces()` currently allocates three
   game-screen schedules, totaling 41,088 bytes, rather than two. Any revised
   memory tally must also distinguish new allocations from renderer state they
   replace and include the unrastered panel and shared top-line palettes.

50 Hz and the existing 144-line band remain unchanged. No fused converter,
room-file loader, runtime 32/48-colour schedule, or cycling/fade/scroll rewrite
has been installed. The early renderer measurement has failed its proposed
gate; full engine-plus-audio profiling in a reproducible walking scene remains
outstanding, as does hardware validation.

## Reproduce

From the repository root:

```sh
node devtools/atari-ste/tools/scumm-ste-rect-test.mjs
node devtools/atari-ste/tools/scumm-ste-rect-test.mjs --hatari --out devtools/atari-ste/monkey-bar/rect-test-hatari
node devtools/atari-ste/tools/verify-monkey-spectrum-model.mjs
node devtools/atari-ste/tools/monkey-strategy-compare.mjs --frame 0 --out monkey-bar/strategy-32/frame-0
node devtools/atari-ste/tools/monkey-strategy-compare.mjs --frame 15 --out monkey-bar/strategy-32/frame-15
node devtools/atari-ste/tools/monkey-strategy-compare.mjs --frame 30 --out monkey-bar/strategy-32/frame-30
node devtools/atari-ste/tools/monkey-actor-schedule.mjs --out monkey-bar/strategy-32/demand
```
