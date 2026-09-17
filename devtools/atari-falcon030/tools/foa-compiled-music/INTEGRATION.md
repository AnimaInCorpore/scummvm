# FCM1 through the real iMUSE engine

2026-09-15 follow-up. The target is **faithful-sounding soundtrack and
MIDI/iMUSE sound effects**, without prerecorded songs, notes or instruments.
Exact internal arithmetic is not the end goal; preserving audible behavior
is. These event-equivalence checks protect that behavior while the live
synthesizer is developed. They are not an audio-fidelity verdict.

## What now works

`engines/scumm/imuse/imuse_fcm.cpp` implements a real `MidiParser` adapter and
an engine-owned FCM score cache. `Player::start_seq_sound()` can opt into it
for DOS Atlantis Roland resources using the original game sound IDs. All
indexed Roland resources are eligible, including MIDI-driven effects; there
is no song-only whitelist. Non-Roland resources, digital speech and sampled
effects retain their existing paths.

The adapter uses the existing parser base for tempo, active-note handling,
smart jumps, scanning, loops, rollback, pause and track selection. It sends
events to the real iMUSE `Player`, not directly to a hardware channel. iMUSE
continues to own its logical parts, custom instruments, hooks, queued commands,
fades and overlapping players. The normal MIDI device selection is unchanged.

Native FCM SysEx instructions are reconstructed once at cue load into immutable
storage. This first adapter intentionally reuses existing iMUSE handlers,
including their nibble decoding. It is a correctness bridge, not a claimed
fast native-command dispatcher. Immutable payloads matter: smart-jump scans
and failed-jump rollback retain `EventInfo` pointers, so a shared scratch
SysEx buffer would corrupt pending instrument/effect data.

The loader checks container bounds, directory ordering, zero alignment padding
and the SCOR checksum. Each loaded cue receives event/argument validation.
The original MDhd and SMF headers must match. Non-score bank sections are not
consumed or CRC-validated by this score-only adapter. Before using a package,
run the full compiler/source/bank verification from the main README; matching
headers alone do not prove that an arbitrary package belongs to your game.

This adapter supports format 0/2, up to 120 independently selectable tracks.
It rejects format 1, SMF escape packets, empty/oversized SysEx, unsupported
opaque Roland/iMUSE messages, undersized native commands and tempos that would
produce zero microseconds per parser tick. The compiler can preserve a broader
event vocabulary, but unsupported runtime data is rejected, not dropped.
All resources in the measured game package pass these checks.

## Live synthesis checkpoint

The supplemental `FCMLIVE.PRG` diagnostic consumes the FCM bank on the Falcon
CPU and renders one factory Xylophone ROM-wave partial at the MT-32's native
32 kHz. It applies velocity, pitch bend, early release and exact integer
resampling to the Falcon codec rate. The normal `scummvm.prg` and release
archive are unchanged; this probe is not wired in as the production music
device.

The host comparison passes 300 note cases and 14,400,000 samples against Munt
with zero mismatches. A 16 MHz Falcon Hatari run with speech and a screen
click proves that the FCM package reaches the live renderer and that its
native-frame checksum matches a separately generated host reference. The
latest [resampler optimization](LIVE-RENDERER.md) cuts resampling cost by
33.8% and mix time per generated audio second by 11.7% at 8,192-frame DMA
halves, with six million output frames byte-identical to the prior renderer.
The 20-second run has zero DMA stops but 13 late buffers and a worst finish
margin of -55 ms. A 60-second run at 16,384-frame halves has four late buffers
and a -75 ms worst margin. Both fail the deadline/waveform gate. The slice is
functionally checked but not real-time-qualified; zero stops alone do not
establish continuous audio.

## Verification

The standalone gate links ScummVM's actual `MidiParser`, `MidiParser_SMF` and
FCM implementation. It does not replace the MIDI parser with a test model.

| Check | Result |
| --- | --- |
| Resource coverage | All 204 indexed resources and 257 tracks |
| Driver/state comparisons | 4,349,341 comparisons passed |
| Scenarios | Linear playback; event-firing jump; failed jump with rollback; jump to start; pause/resume; track change; unload |
| Real game opening | 60 virtual seconds, two runs with each parser |
| Actual post-iMUSE events | All 2,305 events, including 171 SysEx, identical with timestamps |
| Game sound starts | 150, 21, 22, 29, 30, in the same order |
| Interactive opening behavior | Queued fade and overlapping players execute in the real engine |
| Selection proof | FCM cue-load diagnostics verified; baseline logs have none |
| Cross compilation | Adapter and both modified iMUSE source files compile for the Falcon build |

The game gate uses a **separate executable**, `scummvm-fcm-test`. Its null
backend advances a virtual clock only at explicit waits. Both parser modes
therefore receive the same clock behavior. The gate verifies the clock banner,
repeats both runs and compares complete post-iMUSE event files byte-for-byte.
It does not merely compare event counts. The selected device is the existing
offline MIDI capture driver, so no physical MIDI messages are sent.

This is a no-input opening, with speech muted. It does not exercise every
in-game effect trigger, every branch, save/load or Falcon real-time playback.
The all-resource test covers event delivery for every indexed resource but
does not execute the complete iMUSE state machine for every possible game path.

## Memory and performance limits

This first loader retains the 2,112,000-byte SCOR section once per engine.
Each player owns its current cue and reconstructed SysEx storage; the largest
measured parser buffer is 70,682 bytes. Original game resources remain loaded
by ScummVM too. These are data-buffer sizes, not total in-game heap measurements.
They exclude base-parser objects, allocator overhead and a future synthesizer.

The new adapter object is 7,444 text bytes under the current Falcon build
flags. The supplemental live probe reports 4,658,422 bytes of program static
storage and 5,636,096 bytes of peak heap in the measured 14 MiB Hatari case
with 8,192-frame DMA halves (5,701,632 bytes of heap with 16,384-frame halves);
those figures are diagnostic-executable measurements, not a production build
budget. No physical Falcon qualification has been made. The full game's
existing 68882 assumption is unchanged. Future work includes cue paging and
avoiding duplicate original/compiled score residency after source-identity
validation.

## Reproduce

From this tools directory on the current macOS host (the standalone gate uses
the native linker's dead stripping):

```sh
sh ../foa-faithful-music/build-trace.sh
make -C ../foa-faithful-music/build/headless -j8 -f Makefile -f "$PWD/parser.mk" -f "$PWD/imuse.mk" fcm-parser-test scummvm-fcm-test
python3 parser-gate.py --game /path/to/atlantis-cd --package build/atlantis.fcm --binary ../foa-faithful-music/build/headless/fcm-parser-test --output build/parser-check
python3 imuse-gate.py --game /path/to/atlantis-cd --package build/atlantis.fcm --binary ../foa-faithful-music/build/headless/scummvm-fcm-test --output build/imuse-check
```

Use new output directories. The compiler/export commands for `atlantis.fcm`
are in [README.md](README.md). Detailed generated logs/traces stay ignored;
[integration-results.json](integration-results.json) retains the checkpoint.
The original `scummvm` executable and its null-backend object are not replaced
by the virtual-clock executable/object.

To build and run the live diagnostic, first build the normal Falcon target,
then invoke the supplemental makefile and gate:

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

The experimental setting is `foa_fcm_score=/absolute/path/to/atlantis.fcm`
in the Atlantis configuration, with an MT-32 device and `native_mt32=true`.
Without the setting, ordinary SMF playback remains selected. No setting has
been added to the user's normal game configuration. Enabling this setting
does **not** create an internal Falcon synthesizer; events still go to the
previously selected MIDI device.

## Next audible gate

Expand the checked one-partial slice into full MT-32 behavior, then compare
musical and effect timbres with the chosen MT-32 reference. Include custom
timbres, velocity response, bends/modulation, early releases, overlapping
parts, voice allocation and reverb. Audition at matched levels, then close the
combined game/audio deadlines on the Falcon. Approximate arithmetic is
acceptable if the audible result remains faithful; dropped layers, altered
effects or premature releases must not be concealed by an average-error score.

No prerecorded fallback or reduced voice limit was added. The current live
probe remains opt-in and diagnostic only. Save/load needs an explicit
real-engine regression test before this path can be considered production.
