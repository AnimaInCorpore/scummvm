# Captured OPL register traces

Post-AdLib-driver register writes of the real game, as `trace-opl.cpp` records them
on a virtual clock (nothing synthesized, no emulator). Each was captured twice and
the two runs were byte-identical. Format: `W <us> <context> <reg> <value>` (hex),
see the header of each file.

| File | Game | Window | Writes | SHA-256 |
| --- | --- | --- | ---: | --- |
| `atlantis-adlib-60s.ev` | Fate of Atlantis, DOS CD | first 60 s, speech muted | 12,247 | `883bae53...` |
| `cruise-adlib-300s.ev` | Cruise for a Corpse | first 300 s | 8,754 | `5726e28e...` |

Cruise's music starts well into the capture and its drums (rhythm register 0xbd)
first sound at 68.6 s of the virtual clock, 654 writes to that register in all, so
the scorers take its window from about 66 s (`--rhythm-from 66`).

Reproduce (2026-10-02, `scummvm-opl-capture` built per `build-capture.sh`; Atlantis
with the SCUMM-only tree, Cruise with the same configure line and
`--enable-engine=sky,cruise` in a second build directory):

```sh
python3 capture-opl.py --game <atlantis-cd> --milliseconds 60000 --output <dir>
# Cruise: an ini with music_driver=adlib, opl_driver=capture, foa_capture_ms=300000
# and a target of engineid=cruise, gameid=cruise, then
# <cruise build>/scummvm-opl-capture -c scummvm.ini <target>
```

The saved configuration of a build made on another machine may name an SDK path
this one lacks; pass `CXXFLAGS` to make with `-isysroot` pointing at one that exists.

Used by `ablation-study.py --trace traces/atlantis-adlib-60s.ev --rhythm-trace
traces/cruise-adlib-300s.ev --rhythm-from 66`, and by the kernel and practical gates.
