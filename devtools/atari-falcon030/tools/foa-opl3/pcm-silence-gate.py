#!/usr/bin/env python3
"""Check PCM interpolation after omitted silence on the emulated Falcon DSP.

Send a loud period, a silent period (omitted or explicit zeros), then a quiet
period. Read the first four interpolated PCM words back from the unmodified
DSP image. Both forms of silence must leave zero history for the next refill.
Only the standalone 68030 test host is adapted to send PCM and read it back.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("stream_gate", HERE / "rt-stream-gate.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise SystemExit("oplplay.s changed; update the PCM test host: " + old)
    return source.replace(old, new)


def build_host(output):
    source = (HERE / "m68k/oplplay.s").read_text()
    source = replace_once(source, "        move.l  d0,payload_words", """        move.l  d0,d1
        subq.l  #1,d1
        lsl.l   #2,d1
        tst.l   (a3,d1.l)
        beq     no_pcm_words
        add.l   #192,d0
no_pcm_words:
        move.l  d0,payload_words""")
    reads = "        move.l  #CMD_STREAM_STOP,d0\n        bsr     dsp_exchange\n"
    for i in range(4):
        reads += (f"        move.l  #${0x060c00 + i:06x},d0\n        bsr     dsp_exchange\n"
                  f"        move.l  d0,result_pcm+{i * 4}\n")
    source = replace_once(source, "        ; the gate quits the emulator at the stop command: write first", reads)
    source = replace_once(source, "Fwrite  file_handle,#8,result_status", "Fwrite  file_handle,#24,result_status")
    source = replace_once(source, "        Cconws  txt_written\n        move.l  #CMD_STREAM_STOP,d0\n        bsr     dsp_exchange",
                          "        Cconws  txt_written\n        move.l  #$080000,d0\n        bsr     dsp_exchange")
    source = replace_once(source, "result_checksum: ds.l 1", "result_checksum: ds.l 1\nresult_pcm:     ds.l 4")
    assembly = output / "oplplay-pcm.s"
    assembly.write_text(source)
    subprocess.run([str(GATE.VASM), str(assembly), "-quiet", "-Felf", "-m68030",
                    "-I", str(GATE.MXDRV / "src/m68k"), "-I", str(HERE / "dsp"),
                    "-o", str(output / "oplplay.o")], check=True)
    subprocess.run([str(GATE.VLINK), str(output / "oplplay.o"), "-b", "ataritos", "-s", "-e", "start",
                    "-o", str(output / "OPLPLAY.TOS")], check=True)


def words(values):
    return b"".join(struct.pack(">I", value & 0xffffff) for value in values)


def run_case(output, explicit, sign):
    label = ("positive" if sign > 0 else "negative") + ("-explicit" if explicit else "-omitted")
    case = output / label
    case.mkdir()
    shutil.copy(output / "OPLPLAY.TOS", case)
    (case / "empty.ev").write_text("")
    subprocess.run([str(HERE / "build/headless/opl-rt-fixture"), "trace",
                    str(case / "OPLDATA.BIN"), str(case / "EXPECT.BIN"),
                    "--trace", str(case / "empty.ev"), "--seconds", "0.05"],
                   check=True, capture_output=True)
    play = struct.pack(">II", 0x4f504c50, 3)
    play += words([0, 1] + [sign * (16000 << 7)] * 192)
    play += words([0, 1] + [0] * 192 if explicit else [0, 0])
    play += words([0, 1, sign * (1 << 7)] + [0] * 191)
    (case / "PLAYDATA.BIN").write_bytes(play)
    symbols = GATE.listing_symbols(HERE / "dsp/OPLRT.LST")
    (case / "start.ini").write_text(
        f"db pc = ${symbols['profile_end']:04x} :once :trace :file {case / 'end.ini'}\n")
    (case / "end.ini").write_text("quit 0\n")
    with (case / "debug.log").open("w") as log:
        subprocess.run([str(GATE.HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
                        "--conout", "2", "--tos", str(GATE.TOS), "--patch-tos", "true",
                        "--fast-boot", "true", "--fast-forward", "true", "--sound", "off",
                        "--confirm-quit", "false", "--run-vbls", "6000",
                        "--parse", str(case / "start.ini"), "--log-file", str(case / "hatari.log"),
                        "OPLPLAY.TOS"], cwd=case, stdout=log, stderr=subprocess.STDOUT, check=True,
                       env=dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy"), timeout=180)
    status, checksum, *pcm = struct.unpack(">6I", (case / "RESULT.BIN").read_bytes())
    expected = [sign * value & 0xffffff for value in (32, 64, 96, 128)]
    result = {"scenario": label, "rendered": status & 0xfff, "late": status >> 12,
              "pcm_stage_words": pcm, "expected": expected,
              "passed": status == 3 and pcm == expected}
    print(json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    build_host(output)
    cases = [run_case(output, explicit, sign) for sign in (1, -1) for explicit in (False, True)]
    result = {"cases": cases, "passed": all(case["passed"] for case in cases),
              "source_sha256": {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                                for name in ("dsp/oplrt.asm", "m68k/oplplay.s", "pcm-silence-gate.py")}}
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
