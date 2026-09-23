# SSI DMA c2p

A chunky-to-planar conversion for the Falcon030 that runs entirely on the
DSP, with no 68030 moving any of the data. DMA playback streams a chunky
8-bit screen into the DSP's SSI receiver through the crossbar, the DSP
converts it, and its SSI transmitter streams the 8-plane words into DMA
record. This uses the DSP and the sound DMA, so it cannot run beside the DSP
OPL.

The route uses four stereo tracks each way, so all eight 16-bit slots of the
crossbar frame carry data. That makes one crossbar frame exactly one group:
16 chunky pixels in, 8 plane words out. Two crossbar clocks are tested:

| clock | frames (groups) | throughput, each way | one 320x200x8 screen |
|---|---|---|---|
| 25.175 MHz / 256 / 2 | 49,170 Hz | 786,720 B/s | 81.4 ms |
| 32 MHz / 256 / 2 | 62,500 Hz | 1,000,000 B/s | 64.0 ms |

The Falcon specification keeps the 32 MHz clock from the codec only, and
this route does not use the codec. Whether the DMA-to-DSP link runs at it
is one of the questions a real Falcon has to answer.

There are four programs:

- `SSIECHO.TOS`: the route with a DSP that echoes every word, for the
  route's rate and integrity.
- `SSIC2P.TOS`: the conversion itself.
- `SSIMIX.TOS`: whether sound can share the stream, the DAC playing one slot
  pair.
- `CPUC2P.TOS`: the backend's 68030 c2p on the same screen, for comparison.

## The pass-through: SSIECHO.TOS

For each clock the program reports:

- **window**: both DMAs loop for 2 s while the DSP counts words, frame syncs
  and slot phase errors. It passes when the frame rate is within 0.1 % of
  the model and no slot was missed.
- **pass**: one run of a 256,000-byte pseudo-random pattern (four 8-bit
  320x200 frames) into a record buffer 16 KB longer. It passes when the
  pattern comes back intact at a single offset. The report gives that
  offset and its slot phase, the time until the record buffer is full, and
  up to eight damaged words if there are any.

Both measurements run in supervisor mode with interrupts masked. They time
themselves on MFP Timer C's counter and reach the DSP through the host port
registers.

## The conversion: SSIC2P.TOS

The DSP program (`dsp/ssic2p.asm`) works like this:

- **Receive interrupt:** a fast interrupt stores each received word in a
  32-word ring and transmits the output word at the same ring position.
  Transmit therefore runs in lockstep with receive, 32 slots behind it.
- **Framing:** the host starts each screen with a four-word marker.
  After `CMD_SYNC` the DSP scans for it, then converts every following
  group into the output ring positions of its input.
- **Transpose:** the group conversion is a bit-matrix transpose in four
  merge stages. `gen-c2p.py` generates it and checks the exact instruction
  sequence on a model of the registers it uses, against a direct c2p, for
  2,130 groups, before the build assembles it.

Hatari counts 190 instruction cycles per group and 4 per slot for the
interrupt. That is about 222 of the 325 cycles a group lasts at 49,170 Hz,
and of the 256 it lasts at 62,500 Hz.

The 68030 program streams the marker, one pseudo-random 320x200 screen and
128 words of padding. It checks what record wrote against a 68030 reference
c2p. Each clock gets two passes, and each pass reports:

- **pass line:** groups converted, the DSP ring's deepest backlog, receive
  overruns, whether the DSP found the marker, and whether record's
  end-of-DMA interrupt came (`irq 1`).
- **planar line:** where the planar screen starts in the record buffer and
  whether every word is right. The offset must be the same in both passes,
  because a screen driver would rely on it.

The record buffer of each pass is written as `C2P<clock><pass>.BIN`, and
`c2p-slips.py` maps where a bad one lost step.

## Sound beside the stream: SSIMIX.TOS

Converting on the DSP takes the DSP and the sound DMA away from the DSP OPL,
and from all other sound. The idea tested here is slot sharing: sound in one
slot pair of the crossbar frame, data in the other three, and the DAC
listening to that pair alone. `Setmontracks` selects the pair.

The DAC cannot use the 32 MHz clock, so any route with sound runs at
25.175 MHz. With one pair given to sound, 6 of the 8 slots are left for
pixels: 590 KB/s instead of 786 KB/s.

The DSP (`dsp/ssimix.asm`) sends a test tone in the pair the host names:
1,024 Hz left and 2,049 Hz right, from a 48-entry sine. Every other slot
carries a tag, `$s0a5` for slot s. The 68030 program runs five cases:

| case | route | tone | DAC track | record |
|---|---|---|---|---|
| 1 | DSP transmit to record and DAC | pair 0 | 0 | 4 tracks |
| 2 | DSP transmit to record and DAC | pair 1 | 1 | 4 tracks |
| 3 | DSP transmit to record and DAC | pair 0 | 1 (control) | 4 tracks |
| 4 | DMA playback to the DSP and the DAC | pair 0 of the play buffer | 0 | 4 tracks |
| 5 | DSP transmit to record and DAC | pair 3 | 3 | 3 tracks |

Case 5 is the layout a DSP c2p with sound would use. Record would take
planar words from slots 0-5, and the DAC alone would play the sound in slots
6-7.

For each case the program:

- takes a record snapshot and maps which slot every word landed in, by its
  tag;
- checks that the tone pair holds the tone and every other slot its tag.

`mix-gate.py` records Hatari's sound and finds the cases by the silences
between them. It then measures how much of each channel's energy is the
expected tone.

On 2026-09-23, under Hatari:

- **Cases 1, 2 and 4:** the DAC played both tones at 0.998-0.999 (left) and
  0.994-0.995 (right) of the channel's energy, at the tone's exact level.
  The record snapshots put every word in its slot.
- **Case 3 (control):** the DAC played no tone.

So Hatari lets the DAC take one pair out of a four-track stream, from the
DSP or from DMA playback, while the other slots carry other data. Two
limits of Hatari's crossbar leave case 5 to a real Falcon:

- It records every slot, whatever the record track count.
- It decodes only DAC tracks 0 and 1, because its mask for the monitor bits
  is `30` where `0x30` is meant.

Case 5 is therefore reported apart ("hardware only") and left out of the
result. Under Hatari it fails exactly as those two limits predict.

The program works around two more things in Hatari:

- **Waiting:** while a case plays, the 68030 sleeps in `STOP`, woken only by
  playback's end interrupt. Busy waits and TOS's own interrupts cost the DSP
  slots there.
- **Playback slot position:** each case plays one whole 2.2 s buffer and
  waits for its end. Hatari counts playback's slot position from power-on
  and never resets it when playback starts, so an aborted playback would
  move pair 0.

On a real Falcon, what to check:

- whether case 5's record snapshot holds six slots a frame, all tags;
- whether its tone is audible, with the DAC on track 3;
- where the DSP's words land after a frame sync, which the snapshots show
  (Hatari: the next slot).

## The comparison: CPUC2P.TOS

`cpu/cpuc2p.c` times the 68030 c2p the ScummVM backend uses (Mikael Kalms'
routines in `backends/graphics/atari/atari-c2p-asm.S`, linked unchanged) on
the same screen, after checking it against a direct c2p. It times the whole
screen, the dirty-rectangle variant at a few sizes, and a plain screen copy
as the floor. All buffers are in ST-RAM, and the system's interrupts run as
they do under ScummVM. `cpu-bench.py` runs it under Hatari. On 2026-09-23
Hatari's timing gave:

| case | 68030 time | per pixel |
|---|---|---|
| c2p, full 320x200 | 50.9 ms | 795 ns |
| c2p rect 160x100 | 13.0 ms | 811 ns |
| c2p rect 64x64 | 3.5 ms | 843 ns |
| c2p rect 32x32 | 0.96 ms | 941 ns |
| copy, 64,000 bytes | 17.5 ms | 274 ns |

Hatari's 68030 is not cycle-exact, so a real Falcon's `CPUC2P.TXT` is the
figure that counts. Against this, the DSP route takes 81.4 ms (25.175 MHz)
or 64.0 ms (32 MHz) of wall time per full screen. The 68030 is left free for
that time, apart from the sound DMA's share of the ST-RAM bus.

## Build and gates

```sh
DOSBOX=/path/to/dosbox ./build.sh
python3 gate.py --output build/echo
python3 c2p-gate.py --output build/c2p
python3 mix-gate.py --output build/mix
python3 cpu-bench.py --output build/cpu
```

All of them use the sibling F030MXDRV and F030Arcade checkouts, as
`foa-opl3` does. From a worktree, point `MXDRV` and `F030ARCADE` at them.
`CPUC2P.TOS` also needs the `m68k-atari-mintelf` cross compiler, which
`$CROSS` names as a prefix; without it, the build skips that program.

Hatari runs the DSP only between 68030 instructions. An instruction that
takes longer than one SSI slot can therefore hand the DSP two words with no
cycles between them. A DBF in a spin loop costs Hatari up to 42 cycles, while
a slot is 40 (25.175 MHz) or 32 (32 MHz); device and bus-contended ST-RAM
reads cost more. The two gates deal with this differently:

- **The echo** drops the first of the two words, so its strict verdict fails
  under Hatari. Its gate checks the mechanics instead: the modelled rates,
  the pattern found, and only single-word repeats, at most 0.1 %. On
  2026-09-23 Hatari delivered 786,719 and 999,999 B/s and dropped 2 and 26 of
  128,000 words.
- **The conversion** cannot afford a lost word, which would shift every
  later group. So during a pass the 68030 STOPs until record's end-of-DMA
  interrupt. Its gate requires the exact result. On 2026-09-23 both clocks
  converted all 4,016 groups with no damaged word, at offset 37 in every
  pass, and the DSP's backlog never exceeded one group.

## On a real Falcon

Copy `build/SSIECHO.TOS`, `build/SSIC2P.TOS`, `build/SSIMIX.TOS` and
`build/CPUC2P.TOS` to the Falcon. Run them from the desktop with no DSP
program or sound player resident. Each takes about 10-15 s and writes its
report beside itself (`SSIECHO.TXT`, `SSIC2P.TXT`, `SSIMIX.TXT`,
`CPUC2P.TXT`); the first two then wait for a key. `SSIMIX.TOS` is also a
listening test: cases 1, 2, 4 and 5 should each sound as two clean tones,
and case 3 should be silent. The last line is `RESULT: PASS` or
`RESULT: FAIL (n checks failed)`. Any failing line points to what broke:

- **frame rate 0** or a **timed-out pass**: the route carries no data at
  that clock.
- **slot errors** in the echo's window: the DSP loop missed slots.
- **damaged words**: the listed samples show whether each word was dropped,
  doubled or changed; for the conversion, `c2p-slips.py` on the `.BIN` dumps
  shows where.
- **backlog** over 24 words or **overruns** in the conversion: the DSP
  cannot keep up at that clock.
- **irq 0** in the conversion: record's end interrupt never came, and the
  pass fell back to polling. The screen is still checked.
