# Actual SCUMM Bar measurement — 2026-09-11

**The current port runs room 28 at 0.208 game frames/second while Guybrush
walks, and 0.262 fps while he stands still. SCUMM's own frame work averages
6.25 million cycles before STE conversion** — about 25x the proposed
250k-cycle engine gate, and 7.8x the entire 802k cycles of a 10 fps frame.
Engine work, conversion and fixed per-frame overhead together come to about
**16M cycles per walking frame**, against about 431k cycles per 100 ms frame
left once the raster has taken its fixed 46.3% share of the CPU.

These are measurements of the actual game running on the STE backend, using
the original room scripts, objects and actors. They replace the earlier
isolated-converter estimate for assessing the complete room budget. The camera
never scrolls in either run, so these results are a lower bound for the SCUMM
Bar benchmark, which also requires camera movement.

## Results

Cycle-exact Hatari: 68000, 4 MiB STE, RGB 50 Hz, TOS 2.06 DE,
8,021,247 cycles/second. Raster enabled, 144 lines, existing per-line 16-colour
schedule carried in the three-write stream. Dirty-rectangle conversion from
the preceding change is active.

Both runs sample 32 complete frames, numbered 8–39 after entering the room.

| Mean cycles per game frame, at the measured frame rate | Walking | Idle |
|---|---:|---:|
| SCUMM engine phase (`scummLoop`), excluding direct raster instructions | 6,245,091 | 6,221,079 |
| STE conversion phase, excluding direct raster instructions | 8,615,080 | 5,481,929 |
| Other foreground: timer catch-up, event polling, screen handoff, benchmark hook | 5,874,426 | 4,719,524 |
| Direct raster/VBL instructions | 17,892,158 | 14,169,737 |
| Total elapsed cycles | 38,626,754 | 30,592,268 |
| **Measured game fps** | **0.208** | **0.262** |
| Aligned pixels submitted to conversion | 18,798 | 11,748 |
| Palette-schedule rebuilds | 0 | 0 |
| New lazy slot-map entries, rounded mean | 7 | 1 |

Even the lowest measured walking engine frame costs 4,353,524 cycles; its mean
is not being driven solely by one load stall.

### Per-frame work versus time-proportional cost

The rows above do not all scale with game frames. The engine and conversion
phases do. The raster and most of the timer work scale with elapsed time: the
slower a frame runs, the more of them it contains. Adding all four rows and
comparing the sum with a frame budget therefore overstates the gap.

- **Raster.** Direct raster instructions take 46.24–46.42% of every sampled
  frame in both runs, and their per-frame cost correlates 1.0000 with total
  elapsed cycles. That is a fixed 3.715M cycles/second, about 74k per 50 Hz
  VBL, regardless of what the game draws.
- **Timer catch-up.** `DefaultTimerManager::handler` runs once per game frame
  and replays every timer tick that has elapsed since the previous frame (see
  [Timer catch-up](#timer-catch-up)). Its hotspots total 4.88M cycles/frame
  walking and 3.88M idle, in proportion to frame duration.
- **Fixed part of "other".** A least-squares fit of the other-foreground row
  against elapsed cycles over the 32 frames gives an intercept of about
  1.1–1.4M cycles/frame walking and 1.4–1.7M idle, with a slope of 10–12% of
  elapsed cycles. The ranges cover fitting against the same frame's duration
  and the preceding frame's.

At 10 fps a frame lasts 802,125 cycles. The raster takes 371.5k of them,
leaving **430.6k**. Timer catch-up at its measured per-second rate would take
about 101k more, although it is cheap to remove. Against that allowance,
measured engine plus conversion work is 14.86M cycles/frame walking and 11.70M
idle. With the fixed part of "other", walking comes to about 16M, roughly 37x
the 430.6k available.

**Budget basis.** 802k is 8,021,247 ÷ 10. The 250k engine gate and the 380k
foreground budget come from earlier planning and are not derived in this
repository. The 380k figure does not follow from the measured raster share,
which leaves 430.6k per 100 ms frame; its basis should be recorded or the
budget restated from these measurements.

**Audio mixing is compiled out.** `ATARI_STE_GAME_ONLY` selects
`AtariSilentMixer`, discards audio streams and makes the mixer update return
immediately. These results therefore exclude any real sample mixing, decoding,
DMA playback or resampling budget. Sound-related engine timers still run and
are visible in the profile. This is the current port's performance, not an
engine-plus-working-audio result.

## What is consuming the cycles

The following are direct instruction costs within the engine phase, averaged
over the walking sample. They exclude callees, so they can be added without
double-counting nested calls:

| Engine hotspot | Cycles/frame |
|---|---:|
| `MajMinCodec::readBits` | 728,198 |
| `Gdi::drawStripBasicH` | 596,484 |
| `Gdi::writeRoomColor` | 550,071 |
| `ScummEngine::drawStripToScreen` | 483,022 |
| `MajMinCodec::decodeLine` | 434,988 |
| `Gdi::drawStripBasicV` | 384,127 |
| `Gdi::drawStripComplex` | 329,431 |

These seven functions account for **3.51M cycles/frame**, about 56% of the
engine-phase total. `MajMinCodec` is the bit reader used by
`drawStripComplex`, so all seven are strip decoding and drawing. The walking
and idle profiles are nearly identical for them (`readBits` costs 728,198
against 728,201), and `o5_drawObject` appears in both. Restoring and redrawing
the room's strips for scripted object animation is therefore substantial engine
work that does not depend on Guybrush. The room does not become cheap merely
because he stops: its scripts and animations continue, and the idle engine cost
is nearly unchanged. The earlier assumption that background restoration comes
free inside the dirty rectangle is not supported by this implementation.

The flat profile attributes cycles to the nearest preceding symbol, including
local assembly labels. `common2` (129,006) and `less256` (89,588) further down
the engine profile are internal labels of the assembly `memmove` and `memset`
routines, not separate functions.

Within STE conversion, `quantizeRow` itself accounts for 5.12M cycles/frame
walking. The next largest cost is chunky-to-planar conversion at 851k
cycles/frame, split across `asm_c2p1x1_4_rect` (358,008) and its internal
labels `c2p1x1_4_rect_start` (316,435), `c2p1x1_4_rect_done` (111,872) and
`c2p1x1_4_rect_pix16` (65,177). Software 32-bit multiplication (769,496), the
per-row surface copy `AtariSurface::copyRectToSurface` (637,727),
`convertRect` (494,025) and pixel-format comparisons (232,107) follow.
Conversion still submits 8,752–26,368 aligned pixels per walking frame, with a
mean of 18,798. This counts submitted pixels; overlapping dirty rectangles can
contribute the same pixel more than once.

### Timer catch-up

Outside the engine and conversion phases, the largest walking costs are
`__udivsi3_internal` (2.25M), `DefaultTimerManager::handler` (1.15M),
`__mulsi3_internal` (0.65M), timer queue reinsertion (0.54M), and
`__umodsi3` (0.53M).

`ScummEngine::waitForTimer` calls `parseEvents`, and the Atari event source's
`pollEvent` calls `OSystem_Atari::update`, then `checkTimers`, then `handler`.
Every frame is late, so `waitForTimer` never sleeps and the handler runs once
per game frame. It then advances each expired slot one interval at a time,
with a 32-bit division and modulo per step. The SCUMM CD and speech timers both
run at 240 Hz and stay installed with the silent mixer, so a 4.8-second walking
frame replays about 2,300 callbacks. A flat profile does not identify callers;
attributing the division and multiplication helpers to this loop is an
inference, consistent with their size.

This is real work, not sleeping, but it is an artifact of running slowly. At
its measured per-second rate it would cost about 101k cycles in a 100 ms
frame. Suppressing or batching those callbacks in the silent build, or removing
the division from the catch-up loop, would remove most of it. Unlike strip
decompression, it is not a structural cost.

## Scene and measurement contract

- The game is DOS-CD Monkey Island, started with `boot_param=28`. This uses
  the game's own boot script. ScummVM's archived [boot-parameter
  documentation](https://lists.scummvm.org/pipermail/scummvm-git-logs/2006-April/026140.html)
  describes the raw-room fallback; room 28 was then confirmed in runtime state
  and screenshots. The screenshot shows the bar fully populated.
- The walking run's benchmark hook issues `Actor::startWalkActor` whenever
  Guybrush has stopped, alternating between camera-centre ±64 at his current
  feet y. The hook runs at the start of `scummLoop`, just before the engine
  marker, so the call itself falls in the other-foreground row. The walk it
  starts, costume decoding, scripts and drawing run through the normal engine
  paths inside the engine phase. Every sampled walking frame has a nonzero
  moving flag. The measured x range is 96–224 and y range 130–135.
- The idle run issues no walking commands. Guybrush remains at (142,130).
- Camera centre stays at x=160 throughout both samples; the 320-pixel viewport
  therefore covers world x=0–319. There is no scrolling, so these runs do not
  cover the camera movement the SCUMM Bar benchmark requires. Scrolling forces
  strip redraws, so a scrolling sample is expected to cost more. This camera
  position is distinct from the offline fixture's chosen camera offset of 160.
  Runtime snapshots report two visible actors in the current room; scripted
  object animation also runs.
- Measurement begins after seven warm-up frames. No sampled conversion
  rebuilds its palette schedule. Both runs use the same executable, and their
  `SCUMMVM.INI` files differ only in `ste_benchmark_walk`.
- Each run has its own GEMDOS HD directory containing a copy of the
  executable, its own `SCUMMVM.INI`, and `savepath=C:\` inside that directory.
  The `MONKEY` and `ATLANTIS` game directories are symlinks to the build tree's
  copies and are only read. The user's staged executable and savegames were not
  replaced.

The benchmark adds opt-in engine/renderer boundary markers and counters.
Hatari breaks at those markers, saves the state and its external cycle counter,
and writes a per-instruction profile for the preceding interval. There is no
cycle-reading code inserted into the raster interrupt and no modified raster
timing. The analyzer removes instructions in the raster/VBL address range from
foreground categories. TOS service work, other interrupts and exception entry
overhead outside that range remain included in foreground costs, so the
engine number is not a pure script-VM-only count.

Known measurement effects, none of which changes the conclusions:

- **Hook cost.** The benchmark hook is not free and lands in the
  other-foreground row. In the idle run it evaluates
  `ConfMan.hasKey`/`getBool("ste_benchmark_walk")` every frame, and
  `hashit_lower` plus `tolower` appear there at about 27k cycles/frame.
- **Interrupt entry.** If Hatari charges MFP interrupt exception entry to the
  interrupted foreground instruction, about 317k cycles/second (44 cycles ×
  144 lines × 50 Hz, roughly 4% of the CPU) belongs to the raster but is
  counted as foreground, making the effective raster share closer to 50%. This
  is an estimate; how Hatari attributes exception cycles has not been checked.
- **Raster range bound.** The analyzer's raster range ends at
  `raster_exit + 16`, but `colorDistance` begins 12 bytes after `raster_exit`,
  so its first 4 bytes are counted as raster. `colorDistance` costs only
  42,947 cycles/frame in total, so the effect is negligible. The bound should
  be the next symbol's address.

**Cross-check:** summed per-instruction cycles equal the external cycle-counter
differences exactly for every sampled frame; the analyzer itself rejects any
segment that differs by more than 128 cycles. Walking totals 1,236,056,132
cycles over 154.098 emulated seconds; idle totals 978,952,572 over 122.045
seconds. The parser uses unsigned hexadecimal timestamps across the
signed-display boundary and rejects missing timestamps or inconsistent totals.

## Consequence for the proposed architecture

The engine gate fails before working audio is added. The engine phase alone
(6.25M cycles) exceeds an entire 10 fps frame (802k) by 7.8x, and the 430.6k
left after the raster by 14.5x. Eliminating conversion, timer catch-up and the
raster entirely would not close that gap, and reducing the raster band alone
cannot either.

Precomputed schedules may improve conversion (`quantizeRow`, chunky-to-planar
conversion and per-row surface setup) and room-change latency, but they do not
address the strip/object decompression in the engine phase. That
decompression, driven by scripted object animation, is the structural problem,
and the next performance work should start from the engine profile. Timer
catch-up should also be fixed, but it is a cheap-to-remove artifact of running
slowly, not a constraint on the architecture. No optimizations were applied
during this measurement.

## Artifacts and reproduction

- Walking: `devtools/atari-ste/monkey-bar/room-profile/walk/summary.json`, `report.txt`,
  `room.png`, `hatari.log`, `state-*.bin`, `segment-*.txt`.
- Idle: the corresponding files under `devtools/atari-ste/monkey-bar/room-profile/idle/`.
- Each directory retains the measured executable and its configuration under
  `HD/SCUMMVM/`, plus symbol offsets and run metadata.
- Measured executable SHA-256:
  `4759386b09212cae95a684ec0f25076581efb5c7d7a737445ed4deca7a46ed64`.
  The `walk` and `idle` `run.json` files predate the harness recording
  `prgSha256`; this hash comes from `summary.json`, computed from the retained
  copy.
- **The executable was rebuilt after these runs.**
  `../scummvm-ste-scene/build-ste-scumm-static4/scummvm.prg` now hashes
  `c1744d5f986e28988c59eef01560cb7732275abce4d48e62f90ceb667d1866a7` and was
  used only by the two-frame `room-profile/hooks-check` run, which is not part
  of these results. The ScummVM changes are uncommitted in that working tree,
  so rebuilding does not reproduce the measured binary. The measured binary is
  the retained `HD/SCUMMVM/SCUMMVM.PRG` in either run directory.

From the repository root after building the STE executable (this builds from
the current ScummVM working tree, not the measured binary):

```sh
node devtools/atari-ste/tools/scumm-ste-room-profile.mjs --first 8 --last 40 --out devtools/atari-ste/monkey-bar/room-profile/walk-rerun
node devtools/atari-ste/tools/scumm-ste-room-profile.mjs --first 8 --last 40 --idle --out devtools/atari-ste/monkey-bar/room-profile/idle-rerun
node devtools/atari-ste/tools/scumm-ste-room-profile-report.mjs devtools/atari-ste/monkey-bar/room-profile/walk-rerun
node devtools/atari-ste/tools/scumm-ste-room-profile-report.mjs devtools/atari-ste/monkey-bar/room-profile/idle-rerun
```

Choose fresh output directories for subsequent runs; the harness preserves
existing measurement directories instead of mixing old and new samples.

`ste_benchmark` and `ste_benchmark_walk` are opt-in diagnostic configuration
keys for this STE build. Normal runs leave the hooks inactive. The complete
STE executable still builds, and the existing host ASan/UBSan renderer
correctness checks pass after adding the hooks.
