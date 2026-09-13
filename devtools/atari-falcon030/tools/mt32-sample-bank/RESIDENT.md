# Resident sustain experiment — 2026-09-13

**32 sounding sample voices passed the emulated Falcon SSI output test,
including a burst of 16 recorded attacks.** Keeping compact sustain tables
in DSP SRAM removes most continuous sample transfers. This is a standalone
wavetable prototype: faithful MT-32 emulation, a complete MIDI player and
32 voices alongside ScummVM have not been demonstrated.

All playback work runs as 68030/DSP56001 code under calibrated Hatari at stock
clocks, with 14 MB and TOS 4.02. Offline bank and test-control preparation
runs on the development computer. There is no external synthesizer at runtime.
No physical Falcon has been tested. The calibrated emulator's host-port timing
and external-memory contention still need hardware verification.

## What passed, and what failed

Output is 16-bit stereo at **24,584.9609375 Hz**. Controls arrive every
64 frames, or 2.6032 ms. Every case keeps 32 voices sounding. Each sustain
has persistent phase, separately changing pitch (within one semitone) and
gain, and its own fixed pan position. Attack voices replace sustain voices;
they are not extra voices.

| SSI/DAC test | Result |
| --- | --- |
| 32 resident sustains | Pass: 131,072 stereo sample words exactly matched |
| 24 resident sustains + 8 continuously streamed voices | Pass: 131,072 words exactly matched |
| 16 recorded attacks for 151 ms, then all 32 resident | Pass: 131,072 words exactly matched |
| 16 continuously streamed + 16 resident voices | Underrun; abort at 227.81 ms elapsed |
| 32 recorded attacks for 151 ms | Underrun; abort at 93.91 ms elapsed |
| Same 32-attack burst with twice the startup reserve | Underrun; abort at 172.59 ms elapsed |

Successful runs deliver **65,536 stereo frames / 2.6657 seconds** with zero
software underruns and zero SSI transmit errors. They cross the 16-bit
consumer-counter wrap twice. Each is a bounded synthetic test, not a long
gameplay stability test. Continuous streaming tests exercise sustained
bandwidth; they do not implement repeated live note allocation.

The default startup reserve is 512 frames (20.83 ms) in a 1,024-frame ring.
The larger case reserves 1,024 frames (41.65 ms) in a 2,048-frame ring.
Elapsed failure times include filling that reserve and are measured from
the first control transfer, after bank loading. A longer reserve postpones
the 32-attack failure; it does not make that case pass.

For a separate, **unpaced render-to-RAM** timing comparison:

| Workload, always 32 voices total | Time per 512 frames | Audio budget |
| --- | ---: | ---: |
| All resident | 13.963 ms | 20.826 ms |
| 4 streamed, 28 resident | 14.897 ms | 20.826 ms |
| 16 streamed, 16 resident | 17.818 ms | 20.826 ms |
| All 32 streamed | 22.430 ms | 20.826 ms |

Each unpaced run verifies all 32,768 output words over 16,384 frames. These
times include host transfers, DSP rendering and stereo download. They exclude
SSI interrupts and DMA recording, so the 16-stream average does not override
its failed SSI test. Clock-paced SSI elapsed time includes waiting for the
codec; it is **not CPU utilization or spare time for the game**. CPU and DSP
profiles overlap and must not be added.

The earlier full-sample packed fixture cost 30.689 ms per 512 frames. The
resident fixture is about 2.2x faster, but changes the sound representation,
controls and workload. This is not a measured 2x optimization of faithful
MT-32 synthesis, for which no complete live baseline was demonstrated.

## What is precomputed

`resident_bank.py` derives a static, band-limited one-cycle waveform from
each complete Munt patch recording. The current three-patch bank has three
keys and two velocities per patch: **18 tables of 1,024 samples**, occupying
18,432 DSP words (55,296 bytes in 24-bit SRAM). Nine tables occupy X memory
and nine occupy Y memory. The external output ring uses another 2,048 words
by default. Program code fits entirely in the 512-word internal bootstrap
area; the tested SSI image uses 404 words. State reserves 128 internal Y words.
The X/Y layout accounts for the Falcon's physical SRAM aliases.

Bank loading takes about **51 ms**, separate from rendering and prefill.
Once loaded, 32 sustains need only 96 host words per 64-frame block: pitch
step and two pan/gain factors per voice. Sustain phase advances on the DSP.
Recorded attacks use the earlier normalized 12-bit format, two samples per
24-bit host word. Eight streamed voices raise the packet to 352 words;
16 raise it to 608, and 32 to 1,120. This peak traffic is the remaining limit.

`resident.py` prebuilds the complete control stream in ST-RAM, including the
scripted pitch/gain changes and packed attack packets. Runtime MIDI parsing,
bank selection and packet construction are therefore **not charged** to
these measurements. Those costs must be included before game integration.
The 18-table footprint does not establish storage or reload behavior for
Atlantis's complete instrument set.

## Sound quality remains a separate gate

The recorded attack retains the patch's initial waveform. The sustain
reduction discards detuned beating, chorus and evolving filter/partial
behavior. Current attack-to-sustain switches are abrupt: there is no crossfade,
and the streamed attack does not follow pitch bend. There is no note-off
release implementation, modulation engine, reverb or voice allocator.

At key 60 / velocity 100, the four-second sustain comparison gives:

| Patch | RMS spectral difference from Munt | Nearest lookup vs offline linear interpolation |
| --- | ---: | ---: |
| Str Sect 3 | 6.97 dB | 43.56 dB signal/error ratio |
| Fr Horn 1 | 11.00 dB | 45.77 dB signal/error ratio |
| Fantasy | 10.28 dB | 45.94 dB signal/error ratio |

The spectral metric uses reference bins above a 60 dB floor and has no
established perceptual pass threshold. It covers sustain only, unlike the
attack/sustain/release metric in the first experiment. The table indicates
that interpolation error is only one part of the approximation. Root tuning
error from the 16-bit phase increment ranges from −1.03 to +1.45 cents over
the tested keys; arbitrary pitch ranges are unqualified.

`build/resident-bank/listen.html` provides Munt, resident and offline-linear
comparisons for all three patches. They have not been manually auditioned;
there is no verified “good sounding” verdict. The mixed timing fixture is
deliberately dense and is not a musical demo.

## Verification and reproduction

After the capture steps in [README.md](README.md), run from this directory:

```sh
python3 resident_bank.py
python3 resident.py
python3 resident.py --attacks 4 --attack-stress
python3 resident.py --attacks 16 --attack-stress
python3 resident.py --attacks 32 --attack-stress
python3 resident.py --ssi --blocks 1024
python3 resident.py --ssi --attacks 8 --attack-stress --blocks 1024
python3 resident.py --ssi --attacks 16 --blocks 1024
python3 resident.py --ssi --attacks 16 --attack-stress --blocks 1024
python3 resident.py --ssi --attacks 32 --blocks 1024
python3 resident.py --ssi --attacks 32 --prefill-frames 1024 --blocks 1024
```

The last three commands are expected to exit with failure on the measured
configuration and write `failure.json`. Tools and overrides are the same
as for `bench.py`. Generated files remain under ignored `build/resident-*`.
The standalone executable is `MIX.TOS`; SSI cases also write `STATS.RAW`.

The DSP sends stereo over SSI to both DAC and DMA recording. The checker
finds startup alignment only at complete stereo frames within a bounded
prefix, then compares **every** sample against independent Python integer
arithmetic. It cannot accept dropped, repeated or channel-swapped samples.
DSP profile counts independently verify resident and streamed sample counts.
Equality is to this sample algorithm, not to the MT-32 reference recording.

On underrun, the renderer consumes any in-flight control packet, stops SSI,
returns an error acknowledgement and final counters, and the host exits the
test. It no longer waits for the consumer counter to wrap while replaying
old ring contents. Failed runs are retained alongside passes in
[resident-results.json](resident-results.json), including source, executable,
input and profile hashes. Raw profiles remain local generated artifacts.

## Recommendation

For 32 independent note voices, continue with **DSP-resident sustain tables
and a limited pool of recorded attacks**. Eight continuous attack streams
and a 16-note attack burst are demonstrated for this fixture. A complete
player needs a measured policy for larger bursts, smooth sustain joins,
musical bank tuning, release handling and MIDI controls. Sound changes and
attack limits should be exposed as quality choices, not hidden voice losses.

For the closest MT-32 sound in Atlantis, pre-render complete cue segments
or a few synchronized stems and retain iMUSE-aware branching and release
overlaps. This preserves more of the original synthesis and avoids arbitrary
32-stream demand, but does not provide 32 freely playable live notes.

Neither approach is yet qualified alongside ScummVM, speech and effects.
Combined worst-period scheduling, memory high-water and physical Falcon
playback remain necessary before promising the full game experience.
