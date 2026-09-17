# Atari Falcon030 target (stock 14 MB machine)

A **game-only** ScummVM build aimed at an **unaccelerated Falcon030: 16 MHz
68030, 32 MHz DSP56001, 14 MB RAM**. The current game executable also assumes
the optional **68882**. It runs the DOS version of
**Indiana Jones and the Fate of Atlantis**, the game every measurement here
was made with, and since 2026-09-17 also the three other DOS games on the
same AdLib driver the DSP synthesizes for: **The Secret of Monkey Island**
(its v5 editions), **Monkey Island 2** and **Day of the Tentacle**. All four
run their openings on the emulated Falcon with the same transport figures
(`tools/foa-opl3/game-results*.json`). The narrow profile leaves room for
the game and faithful music playback.
Current music work compiles MIDI/iMUSE and MT-32 data for live synthesis,
without prerendered music. Complete live synthesis still has to meet the
Falcon's measured budget; a new format alone does not establish feasibility.

Build with `backends/platform/atari/build-falcon030.sh` (output in
`build-falcon030/`). Branch `falcon030-port`, worktree
`/Users/saschaspringer/Work/scummvm-falcon030`, sibling to the STE port on
`ste-port` / `scummvm-ste-scene`.

## Latest checkpoint: 2026-09-15

The [FCM1 compiler/control prototype](tools/foa-compiled-music/README.md)
preserves all 172,501 events in 204 indexed music resources and 257 tracks,
including 3,417 native iMUSE operations and 52 distinct custom timbres. Its
2.72 MB package contains score instructions, specialized instrument data and
original logarithmic ROM waves, with no rendered audio. Independent score
and bank checks pass. An integer ramp matches Munt over 85 million sample
steps; the small runtime probe compiles for a 68030 without an FPU.

An [opt-in real iMUSE adapter](tools/foa-compiled-music/INTEGRATION.md) now
passes 4.35 million parser comparisons over all indexed resources. Two runs
of each parser also produce identical 2,305-event post-iMUSE traces in the
60-second virtual-clock opening, including its fade and overlapping players.
This covers soundtrack and MIDI-effect resource delivery, not every in-game
effect trigger. It is not an internal waveform renderer or a Falcon performance
result. Live synthesis, listening comparisons, save/load regression and
combined game/audio timing remain. The normal game configuration is unchanged.

A supplemental [live renderer](tools/foa-compiled-music/LIVE-RENDERER.md) now
generates one factory ROM-wave partial on the Falcon CPU. Its exact resampler
reduces measured resampling cost by 33.8%, while 300 note cases still match
Munt. Both the 20-second small-buffer and 60-second larger-buffer game runs
miss audio deadlines. Full synthesis and live iMUSE-to-synth routing remain
unfinished; this is an opt-in diagnostic, not the production music device.

## Historical PCM transport checkpoint: 2026-09-13

The [opt-in in-game PCM probe](tools/foa-faithful-music/IN-GAME.md) now streams
49.17 kHz stereo music alongside actual speech and movement for one minute
in calibrated 16 MHz / 14 MB Hatari. With 16,384-frame DMA halves it reports
zero stops and missing PCM, a 30 ms minimum mix-finish margin, and continuous
music in the captured waveform. Mixing takes about 40% of the CPU; maximum
queued latency is about 666 ms. Smaller halves still fail the waveform gate.

The normal executable builds with the faster equal-rate mixer path and
additional audio servicing during graphics work. Replacement music remains
opt-in diagnostic code: iMUSE transition control, whole-game coverage and
real Falcon disk/audio tests are still outstanding. The older size/config
measurements below describe earlier builds; see the checkpoint for current
in-game memory and performance measurements.

## Prerequisite carried over from master

The branch point (`37ad7bf53df`) does not build against this machine's
sysroot: `backends/mixer/atari/usound_compat.h` deliberately `#error`s when
the installed `usound.h` is version 2 or newer, which it is here. The same
one-line change that is currently uncommitted in the master worktree —
`#include "usound_compat.h"` becomes `#include <usound.h>` — is applied on
this branch so the target compiles. It is not a Falcon-specific change and
belongs upstream.

## What already exists, and what this target adds

The mainline Atari backend (`backends/platform/atari/`) already supports the
Falcon; Monkey Island runs there today. Two flavours ship:

| Flavour | Script | Stated minimum |
|---|---|---|
| Atari Full | `build-release.sh` | Falcon, 4 + 32 MB, 68040 |
| Atari Lite | `build-release030.sh` | TT / Falcon, **16 MB** |

The Lite flavour is the 030 one and is already compiled `-m68030`. The gap
this target fills is the memory line: a Falcon030 maxes out at 14 MB, so the
Lite package's stated 16 MB minimum is not reachable on the machine we are
aiming at. `build-falcon030.sh` therefore keeps the Lite compiler settings and
restricts the build to the SCUMM engine.

Measured from the configure run:

- `ENABLE_SCUMM = DYNAMIC_PLUGIN`, every other engine off.
- `ENABLE_SCUMM_7_8` off — both target games are v5.
- `USE_MT32EMU` undefined (see below).
- `DYNAMIC_MODULES` on, so only `scumm.plg` + `detection.plg` are resident.

## Measured

`build-falcon030.sh` configures and builds cleanly with cross-mint GCC 15.2.0.
Segment sizes from `m68k-atari-mintelf-size`, comparing the first (plugin,
all-SCUMM) build against the current static game-only one:

| Build | text | data | bss | total |
|---|---:|---:|---:|---:|
| plugin: `scummvm.prg` | 4,292,246 | 1,866 | 61,238 | 4,355,350 |
| plugin: `scumm.plg` | 1,564,964 | 4 | 1,512 | 1,566,480 |
| plugin: `detection.plg` | 129,936 | 4 | 4 | 129,944 |
| **static game-only** | **4,876,854** | **1,718** | **50,138** | **4,928,710** |

In-game the plugin build carries `scummvm.prg` + `scumm.plg` = 5,921,830
(detection is unloaded after the scan), so the game-only image saves
**993,120 bytes, about 0.95 MiB or 17%**. Code now costs ~4.7 MiB of the
14 MB, leaving ~9.3 MB for ScummVM's heap, the game's resources, screen
buffers and audio buffers. That figure has not been checked against the real
game yet.

Most of that saving is plugin overhead, not engine removal — see below.

## Running it in Hatari

Cycle-exact Hatari from `../F030Arcade/third_party/hatari/build/src/hatari`,
Falcon TOS 4.04 from `../F030Arcade/third_party/tos/tos404.img`, game data from
`../scummvm/assets/atlantis-cd`:

```sh
hatari --tos tos404.img --harddrive HD \
  --machine falcon --monitor rgb --memsize 14 --cpuclock 16 --fpu 68882 --dsp emu \
  --cpu-exact on --compatible on --sound off --fast-boot on --fast-forward on \
  --conout 2 --run-vbls N --parse prg:start.ini --auto 'C:\SCUMMVM\SCUMMVM.PRG'
```

The HD holds `C:\SCUMMVM\SCUMMVM.PRG` (stripped), `C:\SCUMMVM\SCUMMVM.INI`
(from `../scummvm/assets/scummvm-falcon.ini`) and `C:\ATLANTIS` symlinked to
the game data. `--fpu 68882` is **required** — see below. ScummVM's command
line is injected into the program basepage the way
`third_party/hatari/tools/hatari-prg-args.sh` does it, which is how the target
is started without a keypress:

```
setopt dec
w 'basepage+0x80' <length>
l args.bin 'basepage+0x81'
```

Screenshots come from the debugger's `screenshot <path>` command on chained
`b VBL = N :once :trace :file <next>.ini` breakpoints.

## The FPU: it is required, not optional

### Why there is nothing to add


`-m68882` is not a GCC option (the spelling is `-m68881`, covering both parts).
Verified against the local cross-mint GCC 15.2.0:

- `-m68030` selects the `m68020-60` multilib and emits hardware FPU code
  (`fmove.s` / `fsglmul.s` / `fadd.s`); `-msoft-float` instead calls
  `__mulsf3` / `__addsf3`.
- The toolchain has no separate hard-float multilib — `-print-multi-lib`
  lists only the default, `m68020-60`, `m5475` and their
  `mshort` / `mfastcall` variants.

So the existing Lite build is already a 68030 + 68882 build, and this one
inherits that. There is no additional FPU work, and no matching hard-float
libc to link against if a different FP ABI were ever wanted.

### The consequence, found in Hatari

A Falcon030 ships with an **empty FPU socket**; the 68882 is an add-on. Run
without `--fpu 68882`, which is Hatari's default for a Falcon, the MiNT loader
refuses the program outright:

```
This program requires a 68881 or higher
arithmetic coprocessor and cannot be run on this machine.
```

So this image needs a physically fitted 68882, and so does the stock Atari
Lite package, which uses the same `-m68030`. A build that runs on a Falcon
without the coprocessor would need `-msoft-float` and a soft-float multilib
that this toolchain does not have.

## AdLib/OPL on the DSP

A separate [OPL3 investigation](docs/opl3-feasibility.md) assesses rendering
the game's original AdLib arrangement on the DSP56001, reusing F030MXDRV's
FM architecture. It is a source audit, a reproduction of that project's
YM2151 timing measurements, a capture of the game's own register stream, and
a synthesis kernel measured on an emulated Falcon. Nothing has run on
hardware.

The [capture harness](tools/foa-opl3/README.md) records the register writes
ScummVM's real AdLib driver makes while Atlantis runs, in a separate headless
executable on a virtual clock, with repeated runs byte identical. It measures
burst sizes, callback alignment and keyed-voice occupancy. It renders no audio
and reaches no OPL emulator. The normal Falcon build is unchanged.

The same directory holds a two-operator OPL synthesis kernel, bit exact
against Nuked-OPL3 over parameter sweeps and the captured stream, its
DSP56001 transliteration, and a benchmark on an emulated Falcon. The DSP
kernel reproduces the reference word for word at nine and eighteen channels.

**The exact kernel does not fit**: synthesis alone costs 1,025 instruction
cycles per frame for Atlantis's own nine-channel arrangement, 209% of the
32.780 kHz budget. **The practical kernel does.** It renders at the codec's
49.17 kHz, next to the chip's own rate (at 32.78 kHz its aliasing was audible
as blurred instruments), in 64-frame blocks with block-rate envelopes and
LFO, is scored against the exact kernel by a perceptual gate (sustained tones
within a dB and a cent), costs 62% of the budget on the Atlantis stream and
80% with nine feedback FM channels held, word exact against its host
reference on the emulated DSP, and streams through the SSI with no late
period even under that worst case. The Falcon
build embeds it: `opl_driver=atari_dsp` synthesizes the AdLib score on the
DSP, which also carries the mixer's speech and effects, and `game-gate.py`
runs the game with it on the emulated Falcon and records the music. Both
synthesis and delivery run from an interrupt, so the game's loop stalls at
scene changes (seconds) no longer freeze the music. Nothing has run on
hardware. See the same directory's README for every figure.

## MT-32 on the same machine

**Current direction: compiled MIDI/iMUSE and MT-32 data, synthesized live.**
The [FCM1 prototype](tools/foa-compiled-music/README.md) preserves interactive
score instructions and specializes instrument/control data without recording
notes or songs. Its opt-in adapter feeds the real iMUSE player and existing
MIDI device; it does not synthesize audio internally. It documents what remains
to implement and measure. No new music path is enabled by default.

The older [reference and PCM playback experiment](tools/foa-faithful-music/README.md)
captures real post-iMUSE output and provides Munt reference audio and Falcon
transport diagnostics. It is retained as a test tool, not the selected
soundtrack representation. See the historical checkpoint above for its
bounded in-game streaming result.

The [resident sample prototype](tools/mt32-sample-bank/RESIDENT.md) passes
32 sounding voices through emulated Falcon SSI, including a burst of 16
recorded attacks. Larger attack loads fail. It includes listening comparisons,
exact output checks and measured limits; it is not yet a ScummVM MIDI backend.
The [earlier full-sample experiment](tools/mt32-sample-bank/RESULTS.md) measured
roughly doubled streaming throughput with prepacking, but missed the deadline.

The historical [polyphony options assessment](docs/mt32-polyphony-options.md)
evaluates a 2x improvement and a sample-based 32-voice alternative, which is
not the selected approach. Faithful 2x live emulation and 32 partials alongside
the game remain unverified. It also audits the supplied CPU-headroom report.

Requirements, the game's measured demand and the gap are in
[`docs/mt32-requirements.md`](docs/mt32-requirements.md), with the demand
measurement reproducible via
[`tools/foa-mt32-demand.py`](tools/foa-mt32-demand.py).

Historical caveat: the old resource scanner used physical ordinals, truncated
ROL payloads and missed custom timbres. FCM1 corrects the extraction. The
following old demand figures need recomputation through real iMUSE and are
not valid whole-game fidelity or capacity estimates.

That offline resource analysis averaged **2.24 LA partials per note** and
12.70 of the MT-32's 32 partials while music sounds. Aggregate demand is at most eight partials for
**50.1% of rendered time, including silence**. This is a demand threshold,
not measured live playback coverage. Eight approximate DSP partials and two
CPU PCM partials are planning figures from component costs. PCM accounts for
27.2% of partial demand; spare capacity in one processor does not automatically
cover the other. The rhythm part is active only 6.5% of the time, but melodic
timbres read PCM too.

Measured with [`tools/mt32-partials`](tools/mt32-partials/) against Munt and an
MT-32 control v1.07 + PCM ROM pair. The raw cue export omits live iMUSE
semantics; these figures are not a reference rendition of interactive play.

## Existing MT-32 synthesis kernels: still over budget

The goal of synthesising the game's MT-32 music on the same Falcon is tracked
in `/Users/saschaspringer/Work/F030MT32/docs/scummvm-target.md`. That document
measures the current DSP kernels at **4.91x (square) to 6.22x (saw)** the
available DSP time for a full 32-partial pool with reverb and transport,
before any game, graphics or speech work. It also notes that the 14 MB buys
room for assets and tables but no extra cycles per frame.

Consequences for this build:

- `--disable-mt32emu` is the default in the build script. Munt cannot
  synthesise in real time on a 16 MHz 68030, and leaving it in only costs
  size. Flip `MT32EMU=true` in the script if it is ever wanted for
  host-side comparison.
- The integration boundary named in that document is a ScummVM MIDI driver
  feeding a shared audio backend — i.e. something behind the
  `audio/softsynth/mt32.cpp` interface — not a second sound-owning program.
  `backends/mixer/atari/atari-mixer.cpp` currently owns uSound, the DMA
  buffers and Timer A; a Falcon synthesis backend has to share that owner.
- Until such a driver exists, music on this target goes out over `stmidi`
  to a real MT-32, or is disabled. This happens automatically: the defaulting
  in `backends/platform/atari/osystem_atari.cpp:341` is gated on
  `DISABLE_FANCY_THEMES`, which this build sets, so `music_driver` defaults to
  `stmidi` and `opl_driver` to `null` exactly as in the Lite flavour.

## Result: it runs

Fate of Atlantis starts and plays the opening scene on an emulated stock
Falcon030 — TOS 4.04, 14 MB, 16 MHz, 68882, DSP emulation on. Screenshots at
VBL 4000/7000/10000/13000/16000/19000 show the title card over the collection
room, then Indy standing in it, in 256 colours at 768x528 (Falcon RGB
overscan). No bus error, no address error, no exception in the Hatari log; the
only log error is the host's microphone device, which is a Hatari/SDL matter
and not the port.

Before that, `--fpu 68882` was the difference between running and not; see the
FPU section.

**No frame rate has been measured.** The run used `--fast-forward`, so host
wall-clock means nothing, and the opening scene contains scripted waits, so
the VBL spacing above is not a performance figure either. Timing the port
needs something like the STE port's `ste_benchmark` hooks.

## Resolution: already the original

The Falcon runs Fate of Atlantis at its native 320x200 in 256 colours with no
change needed. Videl registers read mid-game confirm it:

| Register | Value | Meaning |
|---|---|---|
| `$FF8210` VWRAP | `0x00A0` | 160 words = 320 bytes = 320 pixels/line at 8bpp |
| `$FF82A8`/`$FF82AA` VDB/VDE | `0x004D` / `0x01DD` | 400 half-lines = 200 lines |
| `$FF8266` SPSHIFT | `0x0010` | 256 colours |
| `$FF820E` line offset | `0x0010` | 16 words = 32 bytes off-screen |

`Screen::reset()` in `backends/graphics/atari/atari-screen.cpp` picks
`TV | BPS8 | COL40` for anything up to 320x200 on an RGB monitor, with no
overscan flag. Decoding a Hatari screenshot agrees: the non-black content is
exactly 640x400, i.e. 320x200 pixel-doubled, centred in Hatari's 768x528
output buffer. The buffer size is Hatari's, not the Atari's.

The framebuffer stride is 352 bytes rather than 320 because `MAX_HZ_SHAKE 16`
(`atari-graphics.h`) pads 16 pixels each side as off-screen margin for SCUMM's
screen shake. It is never displayed. Removing it would cut about 10% of the
per-line memory traffic and break screen shake, which this game uses; that is
a performance trade to measure, not a resolution question.

## Stripping: what worked and what did not

Under `ATARI_FALCON_GAME_ONLY`, removed from `base/main.cpp`,
`engines/dialogs.cpp` and `engines/dialogs.h`:

- `launcherDialog()` and both call sites. Without a target on the command line
  the build warns and exits; returning from the game quits.
- The `GUI::dumpAllDialogs()` call. This theme-debugging feature instantiates
  every dialog there is, and its reference alone held the whole launcher chain
  in the image - removing the launcher call sites *without* it saved 440 bytes.
- `About` and `Return to Launcher` from the in-game menu, with their command
  cases and the About dialog.
- The in-game `Options` dialog (`GUI::ConfigDialog`), its button and command.
  This is what releases `gui/options.o` and with it `GlobalOptionsDialog`,
  `BrowserDialog`, `ThemeBrowser` and `ShaderBrowserDialog`.

Plus `-DDISABLE_LAUNCHERDISPLAY_GRID`, which upstream's Atari Full script
already passes and the 030 one does not.

| Step | Image (dec) | Delta |
|---|---:|---:|
| plugin build, in-game resident | 5,921,830 | - |
| static game-only | 4,928,710 | -993,120 |
| launcher + About + dialog dump | 4,751,518 | -177,192 |
| nine more `--disable-*` options | 4,751,428 | -90 |
| in-game Options dialog | 4,569,520 | -181,908 |

**Total 1,352,310 bytes (1.29 MiB, 22.8%) below the plugin build.**

Re-verified in Hatari after the last step: Fate of Atlantis boots straight into
the game, title card at VBL 4000, gameplay by VBL 19000, no bus error, address
error or exception. Stripped `SCUMMVM.PRG` on the HD image: 4,625,711 bytes.

Cost of the last step: no runtime volume, subtitle or keymap control. Settings
come from `SCUMMVM.INI` only. Revert by undefining `ATARI_FALCON_GAME_ONLY`
around `ConfigDialog`.

### `--gc-sections` does not work on this toolchain

Section garbage collection is inert for `m68k-atari-mintelf`. Verified twice:
`-Wl,--print-gc-sections` on the full link reports **zero** removed sections,
and a three-function test program keeps a trivially unreachable `dead_fn`.
`-ffunction-sections -fdata-sections` does produce per-function sections in the
objects (103 in `gui/about.o`), so the compiler side works and the linker side
does not. Adding the flags cost **+11,810 bytes** of section overhead for no
benefit, so they are not in the build script.

This matters beyond this port: the STE relink script
(`build-ste-scumm.sh` on `ste-port`) passes `-Wl,--gc-sections` too, and is
presumably getting nothing from it either.

The nine extra `--disable-*` options (`16bit`, `scalers`, `taskbar`,
`system-dialogs`, `seq-midi`, `timidity`, `nuked-opl`, `savegame-timestamp`)
are kept for honesty about intent, but together they saved 90 bytes - those
features were not being linked anyway.

### What is left, and why it needs source surgery

**~312 KiB of dead SCUMM families** are still in the image: 159 `v6` opcode
functions, 41 HE symbols, 29 Towns/NES graphics symbols and 182 non-PC player
symbols - 30 objects, 319,172 bytes of text.

No object references their constructors. They are anchored by **vtables that
`engines/scumm/scumm.o` emits** for `ScummEngine_v0` through `v6`, `v60he` and
`v70he`; each vtable references that family's virtual methods, which live in
`script_v6.o`, `gfx_towns.o`, `player_*.o` and so on. Since `scumm.o` defines
`ScummEngine` itself it can never be excluded, and with section GC unavailable
there is no link-time escape. `v0`-`v4` are genuinely needed - `ScummEngine_v5`
inherits down that chain - but `v6`, `v60he` and `v70he` are not.

Removing them means excluding those sources in `engines/scumm/module.mk` and
guarding the class definitions so `scumm.o` stops emitting their vtables. That
is invasive, touches shared engine headers, and can break gameplay in ways a
title-screen smoke test will not catch. Not attempted here.

## What "game-only" buys, and what it does not

`ATARI_FALCON_GAME_ONLY` (in `engines/scumm/metaengine.cpp`) replaces the
generic constructor matrix with four DOS games: `atlantis`, `monkey` and
`monkey2` on `ScummEngine_v5`, `tentacle` on `ScummEngine_v6`. Both classes
were in the link already (see below), so admitting the three extra games
cost under 200 bytes. All four fit in the 14 MB beside the DSP transport at
least through their openings (`tools/foa-opl3/game-results*.json`). The build is static for the same reason: with a
fixed handful of games there is nothing to choose at run time. This mirrors
`ATARI_STE_GAME_ONLY` on `ste-port`; the two should be unified if both ever
land upstream.

**It trims less than the STE comment claims.** Checked with `nm -C` on the
linked image: `v7`/`v8` and the higher HE families (`v71he` upwards) are gone,
but vtables for `ScummEngine_v0` through `v6`, `v60he` and `v70he` are all
still present. `v0`-`v4` are unavoidable — `ScummEngine_v5` inherits from
them — but `v6`, `v60he` and `v70he` are pulled in by references other than
the constructor matrix. Together with the other clearly-unneeded objects
(`he/`, `gfx_towns`, `gfx_nes`, the `player_*` backends) that is 26 objects
and ~267 KB of text still in the image. Removing them needs the STE port's
object-filtering approach, or `-ffunction-sections -fdata-sections` plus
`-Wl,--gc-sections`; neither is done here yet.

What this frees is **host RAM and 68030 cycles**. It does not help the part
that is actually over budget: MT-32 synthesis runs on the DSP56001, and the
budget document is explicit that more RAM "does not increase the cycles
available per frame or the DSP's directly addressable external SRAM". The
4.91x-6.22x overrun is untouched by anything done here.

It is still the right shape for evidence item 3 in that document — a combined
build running game logic, graphics, CD speech/effects and synthesis together
without missed audio periods — because that item is host-side.

## Open

- Actual resident memory on a 14 MB machine with the game's data; the 16 MB
  figure in the Lite readme has not been re-measured for this build.
- Further SCUMM trimming along the lines of the STE relink script, which drops
  `he/`, `gfx_nes`, `gfx_towns`, the `player_*` backends and the non-PC iMUSE
  drivers. Static linking removes some of this for free; the rest would need
  measuring.
- A dedicated `atarifalcondist` packaging target; the script currently reuses
  `atarilitedist`. Note that target needs `unix2dos`, which is not installed
  on this machine, so packaging (not compilation) fails here — the same is
  true of the existing Lite and Full scripts.
