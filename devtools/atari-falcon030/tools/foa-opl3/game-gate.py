#!/usr/bin/env python3
"""Run Fate of Atlantis on the emulated Falcon with the DSP OPL build, record
the DAC output, and check that the DSP transport kept up and that music
came out of it.

The game runs unattended through its opening; the ScummVM build synthesizes
the AdLib score on the DSP (backends/platform/atari/dsp-opl.cpp) and feeds
speech and effects through the same stream. The transport logs its counters
every 512 periods; this gate allows one late period, the first (the kernel
starts transmitting before the host has a period for it), and no protocol
error, and scores the recording: the music must be present at a sane level.
The kernel counts one late per starvation, not per period, so a late is a
freeze of the music, however long; the PCM underrun and extension counts in
the same line are where the game's loop stalls show.

Established: the integrated build boots the DSP, streams, and the game runs
with it under Hatari. Not established: any comparison against the exact
kernel from inside the game (the game's clock is real time here, so no two
runs are alike), or anything on hardware.
"""
import argparse
import hashlib
import json
import math
import os
import shutil
import struct
import subprocess
import time
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
HATARI = Path.home() / "Work/F030Arcade/third_party/hatari/build/src/hatari"
# The game needs TOS 4.04 (4.02 dies in its video mode switch); the kernel benches run on 4.02.
TOS = Path.home() / "Work/F030Arcade/third_party/tos/tos404.img"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--game", type=Path, required=True, help="the game's data directory")
    parser.add_argument("--gameid", default="atlantis", help="SCUMM game id of the build's profile: atlantis, monkey2, tentacle")
    parser.add_argument("--extra", default="CD", help="the target's extra field, e.g. CD, Floppy, or empty")
    parser.add_argument("--binary", type=Path,
                        default=ROOT / "build-falcon030/scummvm-2026.3.1git-atari-lite/scummvm.prg")
    parser.add_argument("--seconds", type=float, default=90.0, help="game time to record")
    parser.add_argument("--speech", action="store_true", help="leave speech on (off keeps the music clean)")
    parser.add_argument("--opl", default="atari_dsp", help="opl_driver for the run (diagnosis: null)")
    parser.add_argument("--no-dsp-audio", action="store_true", help="diagnosis: DMA playback instead of the DSP")
    parser.add_argument("--sound-rate", default="32780", help="Hatari host sound rate, or off")
    parser.add_argument("--monitor", default="rgb", help="Hatari monitor type")
    parser.add_argument("--no-natfeats", action="store_true", help="diagnosis: leave NatFeats off")
    parser.add_argument("--no-cpu-exact", action="store_true", help="diagnosis: default CPU model")
    parser.add_argument("--ini-extra", default="", help="extra lines for the [scummvm] section, semicolon separated")
    parser.add_argument("--no-press", action="store_true",
                        help="calibration: move the cursor for each --click and screenshot, but do not press")
    parser.add_argument("--click", action="append", default=[], metavar="SECONDS:X:Y",
                        help="left-click at game coordinates X,Y once SECONDS of audio have been submitted "
                             "(for a game that waits on a menu, e.g. Monkey Island 2's difficulty screen); repeatable. "
                             "In Hatari's 320x200 screenshots the game's origin is at (60,72) and the scale is 2, "
                             "so game X,Y = ((sx-60)/2, (sy-72)/2)")
    args = parser.parse_args()
    for required in (HATARI, TOS, args.binary):
        if not required.is_file():
            parser.error(f"missing {required}")
    case = args.output.resolve()
    if case.exists():
        parser.error("the output directory exists; keep previous runs")
    case.mkdir(parents=True)
    hd = case / "HD"
    app = hd / "SCUMMVM"
    app.mkdir(parents=True)
    shutil.copy2(args.binary, app / "SCUMMVM.PRG")
    folder = args.gameid.upper()
    (hd / folder).symlink_to(args.game.resolve(), target_is_directory=True)
    ini = ("[scummvm]\ngui_theme=builtin\ngui_renderer=normal\n"
           + f"music_driver=adlib\nopl_driver={args.opl}\nautosave_period=0\n"
           + f"atari_dsp_audio={'false' if args.no_dsp_audio else 'true'}\n"
           + "music_volume=256\nsfx_volume=256\nspeech_volume=256\n"
           + f"speech_mute={'false' if args.speech else 'true'}\nsubtitles=true\n"
           + "".join(line + "\n" for line in args.ini_extra.split(";") if line)
           + f"\n[{args.gameid}]\nplatform=pc\ngameid={args.gameid}\nengineid=scumm\n"
           + f"language=en\nextra={args.extra}\npath=C:\\{folder}\n")
    (app / "SCUMMVM.INI").write_text(ini)
    (case / "args.bin").write_bytes(args.gameid.encode() + b"\0")
    (case / "start.ini").write_text(f"b pc = text :once :trace :quiet :file {case / 'basepage.ini'}\n")
    (case / "basepage.ini").write_text(
        f"setopt dec\nw 'basepage+0x80' {len(args.gameid)}\nl {case / 'args.bin'} 'basepage+0x81'\n")
    fifo = case / "control.fifo"
    command = [str(HATARI), "--machine", "falcon", "--monitor", args.monitor, "--memsize", "14",
               "--cpuclock", "16", "--fpu", "68882", "--dsp", "emu", "--tos", str(TOS), "--harddrive", str(hd),
               "--fast-boot", "on", "--fast-forward", "on", "--sound", args.sound_rate,
               "--confirm-quit", "off", "--conout", "2", "--cmd-fifo", str(fifo),
               "--natfeats", "off" if args.no_natfeats else "on", "--screenshot-dir", str(case),
               "--cpu-exact", "off" if args.no_cpu_exact else "on", "--compatible", "off" if args.no_cpu_exact else "on",
               "--run-vbls", str(int(args.seconds * 50) * 4 + 12000),
               "--parse", "prg:" + str(case / "start.ini"), "--auto", "C:\\SCUMMVM\\SCUMMVM.PRG"]
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    started = time.monotonic()
    with (case / "hatari.log").open("w") as log:
        proc = subprocess.Popen(command, cwd=case, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while True:
                try:
                    descriptor = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                    break
                except OSError:
                    if proc.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("Hatari did not open its control FIFO")
                    time.sleep(0.05)
            os.close(descriptor)

            def send(text):
                with os.fdopen(os.open(fifo, os.O_WRONLY | os.O_NONBLOCK), "w") as control:
                    control.write(text + "\n")

            if args.sound_rate != "off":
                send(f"hatari-path soundout {case / 'output.wav'}")
                time.sleep(0.1)
                send("hatari-shortcut recsound")
            # The emulator runs faster than real time and the game logs
            # nothing while playing, so the transport's own period counter
            # (68.3 periods per second of audio) paces the run; quitting
            # through the control FIFO finalizes the recording.
            target = int(args.seconds * 32780.0 / 480.0)
            clicks = sorted((float(c.split(":")[0]) * 32780.0 / 480.0, int(c.split(":")[1]), int(c.split(":")[2]))
                            for c in args.click)
            deadline = time.monotonic() + 900
            while proc.poll() is None:
                text = (case / "hatari.log").read_text(errors="replace")
                submitted = [int(line.split("AtariDspAudio: ")[1].split()[0]) for line in text.splitlines()
                             if "AtariDspAudio: " in line and " periods submitted" in line]
                while clicks and submitted and submitted[-1] >= clicks[0][0]:
                    # The cursor position is only known relatively: park it in
                    # the top-left corner first, then move to the target.
                    _, x, y = clicks.pop(0)
                    send("hatari-event mousemove -2000 -2000")
                    time.sleep(0.5)
                    send(f"hatari-event mousemove {x} {y}")
                    time.sleep(0.5)
                    if args.no_press:
                        send("hatari-shortcut screenshot")
                        time.sleep(1.0)
                        continue
                    send("hatari-event leftdown")
                    time.sleep(0.3)
                    send("hatari-event leftup")
                    time.sleep(0.3)
                if submitted and submitted[-1] >= target:
                    # A screenshot of where the game got to, then quit.
                    send("hatari-shortcut screenshot")
                    time.sleep(1.5)
                    send("hatari-shortcut quit")
                    break
                if "~OSystem_Atari" in text:
                    time.sleep(1)
                    send("hatari-shortcut quit")
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError("Hatari timeout")
                time.sleep(1.0)
            proc.wait(timeout=30)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            fifo.unlink(missing_ok=True)

    text = (case / "hatari.log").read_text(errors="replace")
    counters = [line for line in text.splitlines() if "AtariDspAudio:" in line and "periods" in line]
    booted = any("kernel booted" in line for line in text.splitlines())
    picked = any("User picked target" in line for line in text.splitlines())
    last = None
    fresh = [line for line in counters if "stale" not in line]
    for line in counters:
        parts = line.split("AtariDspAudio: ")[1].split()
        last = {"submitted": int(parts[0]), "rendered": int(parts[3]), "late": int(parts[5]),
                "protocol_errors": int(parts[7]), "kernel_counters_fresh": "stale" not in line,
                # Periods the interrupt produced without a PCM chunk from the
                # main loop: the game's loop was stalled for longer than the
                # ring holds. Music is unaffected by these.
                "pcm_underruns": int(parts[10]) if len(parts) > 10 else None,
                # Periods the interrupt submitted without timer callbacks
                # because the main loop held a critical section too long
                # (a resource load): the sequencer slipped 14.6 ms each.
                "extended": int(parts[13]) if len(parts) > 13 else None}
    late_max = max((int(line.split("AtariDspAudio: ")[1].split()[5]) for line in fresh), default=None)

    audio = {}
    wav_path = case / "output.wav"
    try:
        with wave.open(str(wav_path), "rb") as w:
            rate, channels, frames = w.getframerate(), w.getnchannels(), w.getnframes()
            data = w.readframes(frames)
    except (OSError, wave.Error):
        data = None
    if data:
        samples = struct.unpack("<%dh" % (frames * channels), data)
        left = samples[0::channels]
        window = rate
        seconds = []
        for start in range(0, len(left) - window + 1, window):
            chunk = left[start:start + window]
            level = math.sqrt(sum(v * v for v in chunk) / window) / 32768.0
            seconds.append(round(20.0 * math.log10(max(level, 1e-9)), 1))
        loud = [s for s in seconds if s > -50.0]
        audio = {"rate": rate, "channels": channels, "seconds": round(frames / rate, 1),
                 "dbfs_per_second": seconds,
                 "loud_seconds": len(loud), "peak_dbfs": max(seconds) if seconds else None,
                 "first_loud_second": next((i for i, s in enumerate(seconds) if s > -50.0), None)}
    result = {
        "date": "2026-09-17",
        "gate": f"{args.gameid} with the DSP OPL build on the emulated Falcon: transport and recorded audio",
        "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        "requested_seconds": args.seconds,
        "wall_seconds": round(time.monotonic() - started, 1),
        "kernel_booted": booted,
        "game_started": picked,
        "counter_reports": len(counters),
        "last_counters": last,
        "late_periods_max": late_max,
        "audio": audio,
        "screenshot": next((p.name for p in sorted(case.glob("grab*.png"))), None),
        "kernel_counter_reports_fresh": len(fresh),
        "late_allowed": 1,
        "passed": bool(booted and picked and last and fresh
                       and late_max <= 1
                       and last["protocol_errors"] == 0 and audio and audio["loud_seconds"] >= 10),
    }
    (case / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "audio"}, indent=2))
    if audio:
        print("audio:", {k: v for k, v in audio.items() if k != "dbfs_per_second"})
        print("dBFS per second:", audio["dbfs_per_second"][:60])
    if not result["passed"]:
        raise SystemExit("the game run did not pass")


if __name__ == "__main__":
    main()
