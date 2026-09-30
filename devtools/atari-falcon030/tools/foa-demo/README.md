# FOA.TOS: the Fate of Atlantis AdLib score on the Falcon030 DSP

A standalone Falcon program. It plays the AdLib music of *Indiana Jones and
the Fate of Atlantis* through the DSP OPL2 kernel the ScummVM Falcon build
uses, without ScummVM, the game data or any file next to it: everything is in
the one `.tos`.

| Inside FOA.TOS | Source |
| --- | --- |
| The DSP program: two-stage loader and kernel | [foa-opl3/dsp/oplrt.asm](../foa-opl3/dsp/oplrt.asm), the image `backends/platform/atari` boots (`oplrt_image.i`) |
| The kernel's tables and operator records (`foa-tables.bin`) | the fixture's upload blocks, which mirror `AtariDspAudio::uploadTables()` |
| The score (`foa-score.bin`) | the register stream of `audio/adlib.cpp` while the game plays its opening, decoded into sample-stamped parameter events of 768-frame periods |
| The player ([foa.s](foa.s)) | the game's transport, on the raw host port |

"Score" here means what the real AdLib driver produced, not the game's
iMUSE resources: the headless capture of [foa-opl3](../foa-opl3/README.md)
runs the actual game, iMUSE and driver on a virtual clock and records every
register write the driver makes (52,351 in 300 s, sound cues 150, 21, 22, 29
and 30). The register decoder of `opl-practical.h` then turns them into the
events `AtariDspOPL` gives the DSP in the game. So the DSP receives what it
receives in the game, minus the speech and effects PCM, which is silent here.

## Playing

The DSP transmits stereo to the DAC at 49.170 kHz. Each period arrives as
the game sends it: `REFILL`, the kernel's `READY`, a paced host-port blast of
the events and the PCM flag, and the acknowledgement, which carries the
kernel's period and late counters. The kernel answers `READY` only when a
half of its SSI ring is free, so the program runs at real time without a
timer. The screen shows the time played, the length (04:59) and the kernel's
late-period count. Any key quits; when the score ends the DSP is rebooted and
the score plays again.

It needs a Falcon030 with a DSP and TOS 4.x; it runs in any video mode.

## Building

```sh
# once, from foa-opl3: the DSP image and the generated tables
DOSBOX=<dosbox.exe> ../foa-opl3/build-dsp.sh
# once: the headless capture and the fixture (see ../foa-opl3/README.md)
../foa-opl3/build-capture.sh
(cd ../foa-opl3/build/headless && make -f Makefile -f ../../kernel.mk opl-rt-fixture)

./build-foa.sh <Atlantis game directory> [seconds]     # records the score, then builds
./build-foa.sh --trace <opl-writes.ev> [seconds]       # or from a trace recorded earlier
```

The result is `build/foa.tos` (about 0.9 MB for 300 s). The tool paths are
found as `build-dsp.sh` finds them (`$MXDRV`, else `~/Work/F030MXDRV`, else
beside the repository); on Windows run it from MSYS2 or Git Bash with
`C:\msys64\ucrt64\bin` on the PATH.

`build/` is ignored on purpose. `foa.tos` contains LucasArts' music, in the
form of the register stream, so it is for your own machine and is not to be
committed or distributed.

## Checked

On the DSP-calibrated Hatari (TOS 4.04, 14 MB, `--dsp emu`), 2026-09-30, with
the 300 s Atlantis capture:

- The whole score plays with the kernel counting 0 late periods
  (`late periods 0` in the status line throughout).
- A recording of the emulated codec (Hatari's AVI capture at 49,170 Hz)
  correlates with the host reference render of the same events at 0.99999997
  over 12 s from the start of the music, the residual being the 16-bit
  quantisation of the DAC, and the two channels are identical (the OPL2 is
  mono). The score is therefore what the host kernel renders, played by the
  DSP.
- The end of the score reboots the DSP and plays again (checked with a 5 s
  score). The quit path was run with the key press simulated in the source;
  the `Cconis`/`Cconin` poll itself was not driven with a real key.

Not established: no run on a real Falcon030, and nothing was auditioned; both
are as in the foa-opl3 records.
