# Monkey Island SCUMM Bar fixture

This directory contains the first implementation milestone from
`devtools/atari-ste/docs/STE_MONKEY_HANDOFF.md`: a repeatable indexed extractor for DOS CD room 28
and a host-side comparison of two 320×144 STE conversion candidates.

The extractor reads the documented sibling data directory without modifying it,
parses `LECF`/`LOFF` instead of relying on a hard-coded room offset, verifies
the XOR 0x69 resource encoding, decodes the room's SCUMM V5 `SMAP` strips, and
writes the original source indices alongside the 256-entry `CLUT` palette.

From the repository root:

```sh
node devtools/atari-ste/tools/monkey-extract.mjs --out devtools/atari-ste/monkey-bar/fixture
node devtools/atari-ste/tools/monkey-compare.mjs --fixture devtools/atari-ste/monkey-bar/fixture --out devtools/atari-ste/monkey-bar/comparison-camera000 --camera 0
node devtools/atari-ste/tools/monkey-compare.mjs --fixture devtools/atari-ste/monkey-bar/fixture --out devtools/atari-ste/monkey-bar/comparison-camera320 --camera 320
node devtools/atari-ste/tools/monkey-compare.mjs --fixture devtools/atari-ste/monkey-bar/fixture --out devtools/atari-ste/monkey-bar/comparison-checks-camera000 --camera 0 --dither checks
```

To build and show the current joint Spectrum candidate in the authorized
F030Arcade Hatari STE, run:

```sh
node devtools/atari-ste/tools/generate-monkey-native.mjs --fixture devtools/atari-ste/monkey-bar/fixture
node devtools/atari-ste/tools/monkey-native-build.mjs
node devtools/atari-ste/tools/monkey-native-emulate.mjs
```

The four-frame contact capture is written to
`devtools/atari-ste/monkey-bar/native-run/monkey-hatari-contact.png`. The scene animation is
composed into the indexed room before each frame is converted with the joint
16-register Spectrum schedule, so the moving pixels participate in palette
selection instead of being pasted over a fixed background palette. This is a
native renderer smoke test, not an integrated SCUMM engine or gameplay
benchmark. It uses the existing STE VBL palette routine and keeps the lower 56
scanlines as a simple GEM-style status panel.

Use `MONKEY_DATA_DIR` or `--data-dir` when the read-only DOS CD data is stored
elsewhere. The extractor accepts a room number, but the comparison dimensions
and camera range intentionally remain specific to the selected room-28 fixture.

The native generator now loads the three room-28 costume resources (24, 26,
and 37) directly from the read-only DOS CD directory, decodes their classic V5
`COST`/Byle-RLE limbs, places them at calibrated room-world anchors, and
composites the complete scene frame before running the per-line Spectrum
conversion. This keeps all costume pixels in the same palette optimization and
preserves sprite-to-sprite draw order. Costume 24 supplies the 31-step motion;
the other two room actors advance through their shorter real animation groups.
The host then evaluates every frame's candidate line palettes against the
complete sequence and reuses one lowest-error 48-word schedule per scanline.
This prevents stationary room pixels from changing color when a costume moves.
The native viewer is still a deterministic scene renderer rather than full
SCUMM actor/script integration.

The fixture also emits every decodable room `OBIM` image under `objects/` as
source-index binaries and palette-applied previews. Those are room objects;
the native COST decoder is the actor path used by the Hatari scene.

The shared-line candidate uses one 16-color RGB12 palette per source row,
selected from the complete 640-pixel row so it remains stable while the camera
moves. The joint Spectrum candidate uses the existing 16-register,
three-write Spectrum slot model. `--dither checks` follows the application’s
Checks (Error Pair) path: nearest 4-bit channel quantization, a second
error-pair color, Oklab-lightness ordering, and checkerboard parity. Metrics
are fixed-point Oklab squared error
against the source after nearest-per-channel 12-bit gamut quantization. They
are renderer-preparation metrics, not STE timing or game throughput results.

## Temporal colour mixing

`monkey-flicker-compare.mjs` evaluates showing each source colour as a pair of
RGB12 colours on two screen buffers that swap every 50 Hz field. The perceived
colour is the pair's linear-light average. Palettes are optimised over every
combination across the full 186-frame animation cycle of the composed scene,
with pairs limited to a maximum Oklab lightness difference between the fields:

```sh
node devtools/atari-ste/tools/monkey-flicker-compare.mjs --dl 0.05,0.1,0.2,1 --out devtools/atari-ste/monkey-bar/flicker-compare
```

For each limit it reports one 16-colour palette with pure alternation, with a
counter-phased checkerboard and as a static checkerboard; two 16-colour
palettes swapped per field; and a palette-free ceiling of any two RGB12
colours. The per-frame RGB12, single-palette, per-line and full Spectrum 512
strategies are the baselines. Errors are measured against the original VGA
colours, per pixel and after a 2×2 box standing in for viewing distance.
Flicker is the field-to-field lightness difference per pixel and after the
same box, where counter-phased checkerboards cancel.

`flicker-metrics.json` holds the table and palettes, `contact-sheet.png`
compares the strategies at `--sheet-dl`, and `lut-*.bin` files map the 256 VGA
indices to palette slot pairs, followed by the VGA palette they were computed
for. The Atari STE ScummVM backend plays them with `ste_mix_lut` and
`ste_mix_pattern` in the `[scummvm]` section. The time average assumes a 50 Hz display; the
lightness limits still have to be judged on a CRT or true 50 Hz display.

The fixture actors use the room `CLUT`, while the running game sets part of
the palette itself, so tables for the port are built from captured frames.
`scumm-ste-frame-capture.mjs` breaks at the renderer marker after each
requested VBL and saves the engine's finished 320×200 frame and live palette;
`--capture` optimises on those frames, verb bar included, instead of the
fixture scene:

```sh
node devtools/atari-ste/tools/scumm-ste-frame-capture.mjs --out devtools/atari-ste/monkey-bar/room28-capture
node devtools/atari-ste/tools/monkey-flicker-compare.mjs --capture devtools/atari-ste/monkey-bar/room28-capture --split 144 --dl 0.1,0.15,0.2,0.25,0.3 --sheet-dl 0.2 --out devtools/atari-ste/monkey-bar/room28-capture/flicker-compare-split
```

`--split 144` gives the verb bar its own palettes and pairs. The port installs
them with one Timer B interrupt after the last room line, and slot 0 stays
black in every palette, so an interrupt that lands a line late is invisible on
the verb bar's black first line.

`--chroma-weight`, `--error-cap` with `--cap-penalty`, and `--quad-texture`
evaluate optimiser variants kept for later: a lower weight for the Oklab colour
(a/b) error, a flat penalty for any colour visibly off however few pixels it
has, and a 2×2 pattern of two pairs per colour. The evaluator also reports
static `texture`, `distinctOutputs` and `coloursOff`. Results and the port
integration are in `devtools/atari-ste/docs/STE_MONKEY_TEMPORAL_MIXING.md`; `hardware-test/` holds
a kit and checklist for judging the mixing on a real STE.

The generated manifests label the remaining work explicitly: no actor costume
capture, foreground masks, subtitles, deterministic engine sequence, or
integrated engine/audio measurement is claimed yet. The Hatari preview is a
static renderer smoke test and does not claim those missing engine behaviors.
