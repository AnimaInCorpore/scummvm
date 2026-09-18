# Monkey Island DOS CD: SCUMM Bar STE benchmark

## Selection and scope

Use room **28**, `bar`, during Part One while the SCUMM Bar is occupied.
Begin with the crowded entrance-side view, then exercise the camera movement
through the room. Target an unaccelerated 8 MHz Atari STE with 4 MiB RAM and
12-bit STE colors. Preserve the original 320x200 game layout.

Prioritize **visual fidelity first**, then optimize to **10 complete scene
updates per second**. Keep the display raster running at its independently
verified 50 or 60 Hz rate. There is no requirement to reach 30 or 60 game updates
per second. Establish the best practical visual reference with host-side
optimization, then reduce runtime cost while preserving that result. Consider
quality reductions only where necessary to meet the 10 fps target within 4 MiB,
and show their visual effect explicitly.

This document selects and specifies the benchmark. It does not represent an
implemented harness, captured animation sequence, or measured STE result.

The Falcon port is active in parallel. Keep this benchmark and its tools in
Spectrum512Painter. Treat the existing sibling scummvm checkout as read-only;
any future instrumentation or STE integration must use a separate checkout or
worktree without switching, rebuilding, or modifying the Falcon checkout.

## Verified source facts

Inspected the local DOS CD resources on 2026-09-10:

- Source directory: `/Users/saschaspringer/Work/scummvm/assets/monkey-cd`.
- `MONKEY.000` SHA-256:
  `8f40364323a755b1b69fa026a4bb4f351cd3bf330cc005d91fa5d77f55cadefe`.
- `MONKEY.001` SHA-256:
  `d9c4098404e73fde279f4a4d3d819ccfa2c7a149c436dc7fce553018084db661`.
- Resource bytes use XOR 0x69; the LOFF entry for room 28 points to file offset
  1,342,104 in MONKEY.001, where the ROOM block begins.
- RMHD gives a **640x144** room and **52 object records**. These records are
  not a count of simultaneously animated actors.
- A CLUT block contains the room's 256-entry RGB palette.
- The CYCL block's payload is `00 00`. This fixture does not establish coverage
  of active room-declared palette cycles; script-driven palette changes must
  still be observed during capture.

The existing visual reference is
`/Users/saschaspringer/Work/scummvm/assets/hatari-shots/scumm-bar-16mhz.png`.
It shows the crowded entrance side with Guybrush, patrons, tables, windows,
and the bottom interface. It is a Falcon screenshot, not evidence of STE
rendering performance. Use original indexed resources and captured engine
state for conversion; do not infer source palette indices from this PNG.

## Palette strategies to compare

The earlier 10-background / 6-sprite allocation was exploratory, not a target
requirement. Do not assume that this split, a permanent layer split, or the
standard three-palette Spectrum schedule is optimal for Monkey Island.

Compare the following candidates on the same source sequences:

1. **Shared 16-color baseline:** choose colors jointly for scenery, actors,
   objects, text, and cursor. Compare a room-wide palette with stable palettes
   for individual rows or vertical bands. The horizontal scrolling baseline
   must retain the same row palettes across camera positions.
2. **Fixed reservations:** sweep the number of registers kept stable across
   horizontal palette writes, including 4, 6, 8, 10, and 12. Keep 10+6 as a
   comparison case. Permit scenery to use the stable entries too; a reservation
   need not prohibit useful color sharing. Record which register identities are
   reserved, since Spectrum write positions depend on the register number.
3. **Joint Spectrum palettes:** use all available slots for the combined scene.
   Compile mappings for the relevant actors and object states into these shared
   palettes. Compare the cost of changing mappings with the image improvement;
   do not assume that precompiled actor bitplanes remain valid at every position.
4. **Room/band-specific reservations:** keep a small set of important colors
   stable over the regions where they are needed, with the remaining entries
   shared and optimized for local content. Include all actors that can share a
   line, subtitles, and cursor movement when determining those regions.
5. **Alternative raster schedules:** compare palettes changed only between
   lines or bands with more frequent changes in difficult areas. Each schedule
   needs its own palette-access model and verified STE routine. Fewer writes
   save game CPU time only if the scheduling makes that time available; replacing
   writes with waits does not accomplish this.

The preferred hypothesis to test is a shared room palette plan with a limited
number of stable colors where motion requires them. The best number may differ
between rooms and bands. Select it from measured visual quality, temporal
stability, rendering time, raster occupancy, and resident memory, rather than
from background/actor pixel counts alone. Among candidates that meet the 10 fps
target and total memory limit, prefer the higher-fidelity result; speed beyond
that target is secondary.

Perform expensive searches on the host. Optimize over actual animation and
camera states, retain palette-index identity for independently animated colors,
and penalize both actor color changes and background recoloring as actors move.
Any palette-family change must account for every affected pixel, including
background pixels outside the actor's dirty rectangle. Bound cached variants
and include a correct fallback for game states absent from the captured sample;
do not precompute an unbounded table of every actor combination and position.

## Why this room

The entrance view combines several figures at overlapping vertical positions,
distinct clothing colors, dark background shades, small details, and foreground
objects. The 640-pixel room width also permits a 320-pixel camera travel range,
so the same fixture can exercise both a stationary crowded view and scrolling.
Measure actual moving pixels and active animations; visual crowding alone does
not establish rendering workload.

The scene occupies 144 rows. Keep the remaining 56 rows of the 320x200 layout
for the original interface, including its action line. Confirm the live SCUMM
virtual-screen boundaries when capturing a sequence. The raster routine should
end at the scene boundary and install the interface palette there.

## Repeatable cases

1. **Stationary camera:** replay the room's normal ambient animation, with
   Guybrush idle and the interface visible.
2. **Walking and occlusion:** walk Guybrush through the accessible crowded area,
   preserving scaling, actor/object draw order, and foreground masks.
3. **Conversation:** include talking animation and subtitles over the scene,
   checking text and cursor colors as well as actors.
4. **Camera travel:** record the game's actual camera movement through the room
   and back. Add a separately labelled stress sweep over offsets 0..320, covering
   all fine-scroll phases and the transitions 15->16 and 16->15.
5. **Palette coverage:** observe palette events in these sequences. Add another
   verified game fixture for active palette cycling if the bar does not supply
   it. Synthetic cycles may test mechanics but must be labelled synthetic.

Capture indexed scene pixels, source palette changes, camera positions, screen
boundaries, and dirty regions. Layer/costume and mask data are needed to evaluate
specialized actor drawing. A sequence of flattened frames can validate output
but does not by itself measure the cost of executing SCUMM or composing actors.

## Hardware scrolling and Spectrum colors

Use STE hardware scrolling as a candidate for camera movement: coarse video
address changes, 0..15-pixel fine scrolling, and line stride for a wide buffer.
At the interface split, select its fixed display address, reset its scrolling
state and stride as required, and install its palette with verified timing.

Hardware scrolling moves stored pixel indices. Spectrum palette writes are
timed relative to the display beam. A pixel that moves across a palette-write
boundary can therefore change color even though its stored index is unchanged.

Compare two explicit paths:

- **Horizontal-scroll baseline:** a palette that is constant across each line
  and stable across camera positions. Different lines may use different
  palettes. This permits horizontal movement without palette-boundary color
  changes, with lower color freedom than full Spectrum.
- **Spectrum candidate:** scrolling-aware conversion with a validated palette
  schedule and any required index corrections or camera-specific cached data.
  Include correction time and cache memory in the result. Sixteen fine-scroll
  phases alone are not proof that every coarse camera position is covered.

Measure hardware scrolling plus color maintenance against redrawing the changed
viewport. Hardware support is an optimization to validate, not a guarantee of
lower total cost for arbitrary Spectrum-encoded backgrounds.

References:

- [STE scrolling implementation and register behavior, The Paranoid / Paradox](https://alive.atari.org/alive12/ste_hwsc.php)
- [Hatari video timing constants](https://hatari.frama.io/hatari/doxygen/video_8h_source.html)
- Local palette access model: `js/imaging/spectrum512-slots.js`.

## Measurements and acceptance

Report complete scene updates per second, raster refresh rate, worst update
time, raster CPU occupancy, dirty pixel/word counts, and total resident memory.
Separate static-background color error, actor color error, and color changes
on corresponding moving pixels. Inspect walking, scrolling, text, and occlusion
visually in addition to checking encoded screen bytes and palette streams.

Use 512 KiB as an initial planning allowance for additional precomputed renderer
tables, not a fixed quality constraint. Measure engine/resources/audio separately
against the total 4 MiB limit and expand the tables if verified spare memory
allows a useful fidelity improvement.
The existing demo raster is 60 Hz; a 50 Hz implementation requires independent
timing verification. Identify the rate used for every performance result.

The gameplay performance target is **10 complete updates per second**, with
engine execution and streamed audio included. This allows five display frames
per scene update at 50 Hz, or six at 60 Hz. Report update-time spikes as well as
average throughput. Replay-only renderer measurements must be reported
separately. Verify in cycle-exact 8 MHz STE emulation and subsequently on real
STE hardware; Falcon results do not establish this target.
