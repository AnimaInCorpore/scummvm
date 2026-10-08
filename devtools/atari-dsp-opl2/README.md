# Experimental Falcon DSP OPL2 backend

This backend keeps ScummVM's AdLib driver and iMUSE interpreter. Their OPL2
register writes go through the practical decoder to the DSP56001 renderer;
speech and effects retain ScummVM's PCM mixer and use the same SSI output.
It does not use an external MIDI player or a General MIDI instrument bank.

Build the Atari backend with `--enable-falcon-dsp-opl`. This option is
disabled by default and requires an Atari MiNT target. Select it explicitly:

```ini
[scummvm]
music_driver=adlib
opl_driver=falcon_dsp
output_channels=1
output_rate=11025
```

Use a Falcon with 14 MB ST RAM and an emulated DSP for emulator testing.
The present `-m68030` executable uses hard-float instructions and also needs
a 68881-compatible FPU (the test profile uses a 68882). A bare Falcon without
that optional FPU needs a separate soft-float build.

## Scheduling and limits

Timer A delivers immutable prepared periods independently of the game loop,
at approximately 1,010 Hz. Each interrupt has a bounded word/poll budget.
It runs only integer transport code: no mixer, iMUSE, allocator, file access,
or XBIOS calls. An assembly entry preserves the integer registers without
touching the interrupted FPU state. Production stays in normal execution.

Sixteen periods can be prepared ahead (about 250 ms). They carry sample-stamped
OPL events and 192 mono PCM samples per 768 codec frames. The codec clock is
25.175 MHz / 512, and the PCM input is one quarter of that rate. PCM rate
conversion uses a rational sample clock and Q15 interpolation, with no more
than three signed 16-bit sample units of interpolation error. Silence avoids
unnecessary mixing, while cached tails from finished channels are retained.

If no period is prepared, delivery supplies a period without new iMUSE events
or PCM. FM oscillators and envelopes continue, rather than replaying the last
waveform. This is an extension, not evidence that iMUSE stayed on schedule:
the sequencer slips during it and unavailable speech is silent. The queue
adds latency, and sustained or long game stalls can still exhaust it.

Debug level 1 reports delivered periods, extensions, DSP late periods, periods
with an active speech channel, and PCM gaps. A PCM gap counts the first
extension following audible PCM, not the duration or every missing sample.
Speech-period counts demonstrate activity, not an audio-quality verdict.

The DSP uses render-ahead, a practical OPL approximation and 32-frame
envelope/LFO controls. This is not chip-exact YM3812 emulation. Emulator
results do not establish physical-Falcon playback or subjective sound quality.

## Source and regeneration

The decoder, mathematical tables, uploader and DSP kernel originate from
F030OPL2 commit `6beb75a`. The loader and image generator come from that
project's DSP toolchain scaffold. The imported code is covered by its GPLv3
COPYING file; ScummVM is GPL-3.0-or-later. Existing source comments and the
historical Motorola equate-file notice are retained. Table comparisons against
Nuked-OPL3 are historical reference work, not results of this integration.
The old ScummVM fork's timer delivery and period queues informed this transport.

Normal builds use the embedded image and need no assembler or sibling checkout.
For a DSP change, assemble the supplied `dsp/oplrt.asm` and
`dsp/stage2_loader.asm` using Motorola ASM56000 and CLDLOD (keeping
`ioequ.inc` and `oplrttab.inc` in the assembler's directory). Then run:

```sh
python3 devtools/atari-dsp-opl2/generate-image.py \
  --bootstrap OPLBOOT.LOD --program OPLRT.LOD \
  --output backends/mixer/atari/opl/dsp-image.h
```

The generated header records the kernel source hash and checks the internal
P-RAM boundary and external P/Y alias region. A DSP change also requires fresh
frame comparisons and stream/rhythm timing gates; assembly alone is insufficient.

Host transport checks (from the repository root):

```sh
g++ -O2 -std=c++11 -DPOSIX -I. devtools/atari-dsp-opl2/stream-test.cpp -o stream-test
./stream-test
```

These check FIFO wrap/full behavior, event boundaries and key-on/off stamps,
long-run rational PCM consumption, and full-scale interpolation error bounds.

`analyze-run.py` extracts the PCM soundtrack from a Hatari AVI and checks
transport counter reports. Its gate requires actual speech-channel activity,
no reported DSP lateness, no reported PCM gaps, and audible recorded output.
It does not gate extensions or establish correct musical tempo during stalls.
Retain the tested binary, launch arguments, recording and log with each result.
