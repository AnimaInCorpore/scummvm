# FCM1: compiled, interactive Fate of Atlantis music

2026-09-15. Active music direction: restructure the game's MIDI/iMUSE and
MT-32 data for live synthesis on an unaccelerated Falcon, without recordings
of songs, notes, attacks or complete instruments. This directory implements
the compiler, bank specialization and control-runtime prototype. An
[opt-in real iMUSE adapter](INTEGRATION.md) now passes all-resource parser
comparisons and a repeated in-game opening comparison. A supplemental
`FCMLIVE.PRG` now also exercises one internal ROM-wave partial through the
Falcon mixer, but **this is a diagnostic slice, not a full-polyphony or
real-time-qualified Falcon synthesizer**.

FCM means Falcon Compiled Music. It moves parsing, nibble decoding, timbre
structure selection and some parameter calculations off the Falcon. It does
not move the musical decisions offline: game-controlled hooks, jumps, fades,
overlapping players, note releases and controller changes must remain live.
The binary contract is in [FORMAT.md](FORMAT.md).

## Implemented and checked

The local DOS CD data compiles into a **2,719,834-byte** package with:

- 204 indexed sound resources, 257 separately selectable tracks, and 172,501
  events, with original ticks, metadata, payloads and same-tick order retained.
- 3,417 native iMUSE operations and 69 custom-instrument events containing
  **52 distinct custom timbres**. The earlier claim of factory patches only
  was wrong. Two custom definitions require Munt's parameter normalization;
  both original and normalized definitions are retained.
- 128 factory melodic timbres, 30 rhythm timbres and those 52 customs, sharing
  557 distinct partial-parameter records and 76 velocity-response tables.
- Preselected square/saw/ROM-wave partial descriptors, pair relationships,
  resonance constants, and integer envelope increment tables.
- The MT-32's original logarithmic wave-ROM words, only bit-unpacked into
  big-endian words. These are synthesis source data, **not prerendered notes
  or linear PCM audio**. Synthetic partials still require live LA generation.

All 172,501 events pass comparison against a second SMF event reader. A native
C++ consumer independently checks record decoding, track cursors and seeking.
An independent C++ bank check reconstructs all 210 normalized timbres and
checks all 71,296 compiled velocity values and resonance constants using Munt's
named parameter fields and tables. Eleven unit tests cover malformed input,
resource IDs, format-2 tracks, opaque SysEx and native command reconstruction.

`runtime.h` contains an allocation-free track cursor and an integer affine
ramp with delayed completion interrupts. The ramp passes **85,387,454 sample
steps** against Munt's `LA32Ramp`, covering all 65,536 target/increment pairs,
retargeting, sustained values and batched advancement. This is reference-model
agreement for that primitive, not proof of complete envelopes or audio fidelity.
The control probe compiles with `-m68030 -msoft-float`: 534 text bytes, no
unresolved symbols. The live diagnostic compiles for the same target and its
host kernel/reference suite passes: 1,152,000 kernel samples and 14,400,000
Munt-comparison samples, with zero mismatches across 300 note cases. It covers
one factory Xylophone ROM partial, velocity, bends, early release and the
32 kHz-to-Falcon-rate resampler; it does not cover full MT-32 synthesis.

[results.json](results.json) records this checkpoint without game or ROM data.
It describes the initial compiler checkpoint; subsequent parser integration
results are in [integration-results.json](integration-results.json). Generated
packages, bank dumps and detailed reports stay under ignored `build/`.

## Why this is useful, and what it cannot solve

The score representation is deliberately larger than MIDI: 780,602 source
ROL payload bytes become 2,112,000 SCOR bytes. Fixed-size records avoid runtime
VLQs and running status and permit direct track access. This is a CPU-oriented
format, not an audio codec or a compression claim. The largest individual cue
is 69,263 bytes; a future loader can page cues instead of retaining the entire
score. Actual in-game memory, DSP cache traffic and CPU cost remain unmeasured.

The ramp represents a current value, step and completion time instead of
repeating increment decoding. Velocity-dependent initialization becomes a
lookup for any velocity, not just velocities observed in one playthrough.
This specialization is valid across changing interactive paths. It does not
remove the cost of producing each audible sample, ring modulation, reverb,
voice allocation or sample transfer. No measured synthesis-speedup or promise
of 32 live partials follows from the package size or control tests.

The live game probe loads the FCM package, synthesizes that diagnostic partial
on the target CPU, and proves the package identity plus native-frame checksum
against a separately generated host reference. The latest
[exact resampler optimization](LIVE-RENDERER.md) reduces resampling cost by
33.8% and mix time per generated audio second by 11.7% in the 8,192-frame
Hatari case. Six million output frames remain byte-identical. At 16 MHz with
speech and a screen click, the 20-second run now has zero DMA stops but 13
late buffers, with a worst finish margin of -55 ms. A 60-second run with
16,384-frame halves has four late buffers and a -75 ms worst margin. Both
still fail the deadline/waveform gate; no production qualification follows.

## Build and reproduce

Use Python 3.8+ and a C++11 host compiler. The default Munt location is the
sibling `F030MT32/third_party/munt`; override `MUNT` if necessary. Host Munt
objects are built through the existing `../mt32-partials` Makefile. Munt is
linked only into the exporter/oracle, never the target runtime. The exporter
requires recognized complete `ctrl_mt32_1_07` and `pcm_mt32` ROMs.

Run from this directory, substituting your own game and ROM paths:

```sh
make test
python3 fcm.py extract-custom --game /path/to/atlantis-cd --output build/custom.bin
build/oracle export /path/to/mt32_ctrl_1_07.rom /path/to/mt32_pcm.rom build/bank.fmb build/custom.bin
python3 fcm.py compile --game /path/to/atlantis-cd --bank build/bank.fmb --output build/atlantis.fcm
python3 verify_game.py --game /path/to/atlantis-cd --package build/atlantis.fcm --bank-dump build/bank.fmb --report build/verification.json
python3 fcm.py inspect build/atlantis.fcm
make target-check MINT_CXX=/path/to/m68k-atari-mintelf-g++
```

The supplemental target is built after the normal Falcon target:

```sh
env PATH=/path/to/cross-mint/bin:$PATH make -C ../../../../build-falcon030 \
  -f Makefile -f ../devtools/atari-falcon030/tools/foa-compiled-music/live.mk \
  fcm-live-probe
make build/live-demo
make live-test
python3 live-game-gate.py build/atlantis-v3.fcm \
  --output build/live-game-check --samples 8192 --delay-ms 300000 \
  --duration-ms 20000 --speech --click 40 80
```

Outputs must be new filenames: commands refuse to overwrite previous packages,
bank dumps or verification reports. Omitting `--bank` produces a score-only
package, useful for structural tests, not sufficient for synthesis. `inspect`
checks the container and score; use `verify_game.py --bank-dump` for the source
comparison and independent compiled-bank checks. The exporter does not call
Munt's audio renderer. Generated packages contain game/ROM-derived data; do
not commit or distribute them as project fixtures.

## Runtime contract and next implementation gates

1. **Keep the real iMUSE engine.** The new adapter reconstructs its existing
   event interface from FCM records. A player owns
   its selected-track cursor; format-2 tracks must not be merged. Tick-to-time
   conversion still follows current tempo and player state. The parser and
   opening gates pass; extend actual post-iMUSE comparisons to more game inputs,
   effect triggers and save/load. Only then replace proven hotspots with direct
   native-op dispatch. See [INTEGRATION.md](INTEGRATION.md) for the exact scope.
2. **Keep live MT-32 control state.** Program changes, custom instruments,
   sustain, note-off, pitch bend, modulation, mutable patch memory and the
   original 32-partial allocation policy remain runtime operations. A custom
   event targets an iMUSE logical part, not an already allocated MIDI channel.
   ROM defaults do not replace ScummVM's reset/system/rhythm/bender writes.
   Timbre mutations must invalidate affected compiled constants; unsupported
   mutations must be reported, never silently ignored.
3. **Expand the live renderer.** The current supplemental probe is only one
   factory ROM-wave partial. Add square/saw kernels, custom timbres, multiple
   partials and iMUSE-owned voice allocation while preserving ring-pair
   semantics, phase, interpolation, releases, modulation and reverb. Keep the
   MT-32's 32 kHz synthesis clock distinct from Falcon codec rates.
4. **Close the combined budget.** The current one-partial diagnostic is not
   deadline-qualified at 16 MHz. Measure smaller and larger audio buffers with
   graphics, speech, dense passages, transition overlaps, note-off storms and
   disk stalls; compare live output to fixed Munt profiles. A lower voice cap
   or changed reverb is a fidelity change, not a transparent fix.

`Ramp::advance(n)` is equivalent to advancing one unchanged ramp over that
span. A controller must split at its next relevant envelope interrupt, note or
controller event and then select the next segment; it cannot skip a transition
and reconstruct it after the block. Likewise, `Track::seek()` only moves an
event cursor: it does not restore controllers, sounding notes or synth state.
Saving a ramp alone is not a complete music save state.

The new primitives require no FPU. The existing game executable still assumes
the optional 68882 through its current toolchain setup; this work does not
establish that the complete game runs on an FPU-less stock Falcon. Default
parser selection and audio ownership remain unchanged; FCM playback is opt-in.

The older [PCM reference/transport experiment](../foa-faithful-music/README.md)
remains useful for comparison and audio-transport diagnostics, not as the
selected music representation. Its captured opening is not whole-game coverage.

## Source anchors

- Resource lengths/indexing: `engines/scumm/resource.cpp` and
  `engines/scumm/sound.cpp`, `readSoundResource`; DSOU + LOFF supply real IDs.
- Logical-part custom timbres: `engines/scumm/imuse/imuse_player.cpp`,
  `Player::sysEx`; native commands: `engines/scumm/imuse/sysex_scumm.cpp`.
- Synthesis definitions: Munt `Structures.h`, `Tables.cpp`, `Partial.cpp`,
  `LA32WaveGenerator.cpp` and `LA32Ramp.cpp` at revision recorded in results.

The historical signature scanner counted 210 physical ROL chunks, including
duplicate copies, and truncated each payload by eight bytes. This compiler
uses 204 directory-indexed resources and the full payload lengths. Its
verification reuses only the historical SMF event reader, not that scanner.
