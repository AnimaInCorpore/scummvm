# SSI DMA pass-through test

The first step of the DSP c2p feasibility test. The idea is to convert a
whole chunky screen on the DSP without the 68030 moving any of the data:
DMA playback streams the frame into the DSP's SSI receiver through the
crossbar, and the SSI transmitter streams the result into DMA record. This
uses the DSP and the sound DMA, so it cannot run beside the DSP OPL.

This step asks whether the route itself works and how fast it is.
`SSIECHO.TOS` boots a DSP program that echoes every SSI word unchanged,
routes four stereo tracks each way (all eight 16-bit slots of the crossbar
frame), and checks two crossbar clocks:

| clock | frame rate | model throughput, each way | one 320x200x8 frame |
|---|---|---|---|
| 25.175 MHz / 256 / 2 | 49,170 Hz | 786,720 B/s | 81.4 ms |
| 32 MHz / 256 / 2 | 62,500 Hz | 1,000,000 B/s | 64.0 ms |

The Falcon specification keeps the 32 MHz clock from the codec only, and
this route does not use the codec. Whether the DMA-to-DSP link runs at it
is one of the questions a real Falcon has to answer.

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

## Build and gate

```sh
DOSBOX=/path/to/dosbox ./build.sh
python3 gate.py --output build/run
```

Both use the sibling F030MXDRV and F030Arcade checkouts, as `foa-opl3` does.
From a worktree, point `MXDRV` and `F030ARCADE` at them.

Hatari runs the DSP only between 68030 instructions. Any instruction slower
than one SSI slot, such as an MFP, sound register or bus-contended ST-RAM
read, can therefore hand the DSP two words at once, and the echo drops the
first. The program's strict verdict fails under Hatari for that reason. The
gate checks the mechanics instead: the modelled rates, the pattern found,
and only single-word repeats, at most 0.1 %. On 2026-09-23 Hatari delivered
786,719 and 999,999 B/s and dropped 2 and 26 of 128,000 words.

## On a real Falcon

Copy `build/SSIECHO.TOS` to the Falcon and run it from the desktop with no
DSP program or sound player resident. It takes about 10 s, writes
`SSIECHO.TXT` beside itself and waits for a key. The last line is
`RESULT: PASS` or `RESULT: FAIL (n checks failed)`. Any failing line points
to what broke:

- **frame rate 0** or a **timed-out pass**: the route carries no data at
  that clock.
- **slot errors** in the window: the DSP loop missed slots.
- **damaged words** in the pass: the listed samples show whether each word
  was dropped, doubled or changed.
