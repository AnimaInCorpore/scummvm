# Atari Falcon030 target (stock 14 MB machine)

A **game-only** ScummVM build aimed at a **stock Falcon030: 16 MHz 68030 +
68882, 32 MHz DSP56001, 14 MB RAM**, running the DOS CD version of
**Indiana Jones and the Fate of Atlantis** — and nothing else. The single-game
restriction is deliberate: on-machine MT-32 synthesis is the expensive half of
this target, so the ScummVM half gives up everything it can.

Build with `backends/platform/atari/build-falcon030.sh` (output in
`build-falcon030/`). Branch `falcon030-port`, worktree
`/Users/saschaspringer/Work/scummvm-falcon030`, sibling to the STE port on
`ste-port` / `scummvm-ste-scene`.

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

## MT-32 on the same machine

Requirements, the game's measured demand and the gap are in
[`docs/mt32-requirements.md`](docs/mt32-requirements.md), with the demand
measurement reproducible via
[`tools/foa-mt32-demand.py`](tools/foa-mt32-demand.py).

Headline, now measured rather than projected: the game costs **2.24 LA
partials per note** and a mean of 12.70 of the MT-32's 32 partials while music
sounds, so the best measured Falcon budget (8 approximate partials) covers
**50.1%** of playing time without stealing voices. The rhythm part, which is
the PCM-partial path, is active only 6.5% of the time.

Measured with [`tools/mt32-partials`](tools/mt32-partials/) against Munt and an
MT-32 control v1.07 + PCM ROM pair.

## MT-32 emulation: not yet feasible

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
generic constructor matrix with a single v5/DOS/`atlantis` path. The build is
static for the same reason: with one game there is nothing to choose at run
time. This mirrors `ATARI_STE_GAME_ONLY` on `ste-port`; the two should be
unified if both ever land upstream.

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
