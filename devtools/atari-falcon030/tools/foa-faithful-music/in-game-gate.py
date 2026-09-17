#!/usr/bin/env python3
"""Run the opt-in Falcon PCM probe through the game's normal mixer in Hatari.

Input is signed big-endian PCM16 stereo, already resampled to 25175000/512 Hz.
GEMDOS directory reads do not simulate physical Falcon disk throughput.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pcm", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--live-fcm", action="store_true", help="Input is an FCM bank; run FCMLIVE.PRG's live Xylophone diagnostic")
    parser.add_argument("--game", type=Path, default=ROOT.parent / "scummvm/assets/atlantis-cd")
    parser.add_argument("--hatari", type=Path, default=ROOT.parent / "F030Arcade/third_party/hatari/build/src/hatari")
    parser.add_argument("--tos", type=Path, default=ROOT.parent / "F030Arcade/third_party/tos/tos404.img")
    parser.add_argument("--resident", action="store_true")
    parser.add_argument("--speech", action="store_true")
    parser.add_argument("--click", type=int, nargs=2, metavar=("X", "Y"), help="Click native game coordinates two seconds into measurement")
    parser.add_argument("--delay-ms", type=int, default=20000)
    parser.add_argument("--duration-ms", type=int, default=60000)
    parser.add_argument("--samples", type=int, default=2048)
    parser.add_argument("--ring-bytes", type=int, default=262144)
    parser.add_argument("--chunk-bytes", type=int, default=32768)
    parser.add_argument("--cpuclock", type=int, choices=(16, 32), default=16)
    args = parser.parse_args()
    probe = HERE.parent / "foa-compiled-music/build/FCMLIVE.PRG" if args.live_fcm else HERE / "build/PCMTEST.PRG"
    symbols = HERE.parent / "foa-compiled-music/build/live-symbols.txt" if args.live_fcm else HERE / "build/probe-symbols.txt"
    if not args.pcm.is_file() or not probe.is_file() or not symbols.is_file():
        parser.error("Build the selected probe and provide an existing input file first")
    if not args.game.is_dir() or not args.hatari.is_file() or not args.tos.is_file():
        parser.error("Game directory, Hatari and TOS must exist")
    if not 512 <= args.samples <= 16384:
        parser.error("Use 512 through 16384 frames per DMA half")
    if not 0 <= args.delay_ms <= 600000 or not 1000 <= args.duration_ms <= 120000:
        parser.error("Use delay 0..600000 ms and duration 1000..120000 ms")
    if not 16384 <= args.ring_bytes <= 4194304 or args.ring_bytes % 4:
        parser.error("Ring size must be 16384..4194304 bytes in whole stereo frames")
    if not 4096 <= args.chunk_bytes <= args.ring_bytes or args.chunk_bytes % 4:
        parser.error("Chunk size must be 4096..ring size in whole stereo frames")
    size = args.pcm.stat().st_size
    if args.live_fcm and not 12 <= size <= 16 * 1024 * 1024:
        parser.error("FCM package must be 12 bytes through 16 MiB")
    if not args.live_fcm and (not 0 < size <= 0x7fffffff or size % 4 or (args.resident and size > args.ring_bytes)):
        parser.error("PCM must contain whole frames and fit the ring in resident mode")
    if args.click and not (0 <= args.click[0] < 320 and 0 <= args.click[1] < 200):
        parser.error("Click coordinates must be inside the 320x200 game screen")
    case = args.output.resolve()
    if case.exists():
        parser.error("Output directory already exists; preserve previous measurements")
    case.mkdir(parents=True)
    hd = case / "HD"
    app = hd / "SCUMMVM"
    app.mkdir(parents=True)
    shutil.copy2(probe, app / "PCMTEST.PRG")
    shutil.copy2(symbols, case / "symbols.txt")
    shutil.copy2(args.pcm, app / "MUSIC.RAW")
    (hd / "ATLANTIS").symlink_to(args.game.resolve(), target_is_directory=True)
    (app / "SCUMMVM.INI").write_text(
        "[scummvm]\ngui_theme=builtin\ngui_renderer=normal\n"
        "music_driver=null\nopl_driver=null\nautosave_period=0\n"
        "output_rate=49170\noutput_channels=2\n"
        f"audio_buffer_size={args.samples}\nmusic_volume=256\nsfx_volume=256\nspeech_volume=256\n"
        f"speech_mute={'false' if args.speech else 'true'}\nsubtitles=true\n"
        "pcm_probe_file=C:\\SCUMMVM\\MUSIC.RAW\n"
        f"fcm_live_probe={'true' if args.live_fcm else 'false'}\n"
        f"pcm_probe_resident={'true' if args.resident else 'false'}\n"
        f"pcm_probe_delay_ms={args.delay_ms}\npcm_probe_duration_ms={args.duration_ms}\n"
        f"pcm_probe_buffer_bytes={args.ring_bytes}\npcm_probe_chunk_bytes={args.chunk_bytes}\n"
        f"pcm_probe_click_x={args.click[0] if args.click else -1}\npcm_probe_click_y={args.click[1] if args.click else -1}\n"
        "\n[atlantis]\nplatform=pc\ngameid=atlantis\nengineid=scumm\n"
        "language=en\nextra=CD\npath=C:\\ATLANTIS\n")
    (case / "args.bin").write_bytes(b"atlantis\0")
    (case / "start.ini").write_text(
        f"b pc = text :once :trace :quiet :file {case / 'basepage.ini'}\n")
    (case / "basepage.ini").write_text(
        f"setopt dec\nw 'basepage+0x80' 8\nl {case / 'args.bin'} 'basepage+0x81'\n"
        f"symbols {case / 'symbols.txt'} TEXT\n"
        f"b d0 = $50434d31 :once :trace :quiet :file {case / 'begin.ini'}\n"
        f"b d0 = $50434d32 :once :trace :quiet :file {case / 'end.ini'}\n")
    (case / "begin.ini").write_text(
        f"echo PCM_PROBE_BEGIN\ninfo crossbar\nscreenshot {case / 'begin.png'}\nprofile on\n")
    (case / "end.ini").write_text(
        f"profile off\nprofile save {case / 'cpu.txt'}\n"
        f"screenshot {case / 'end.png'}\necho PCM_PROBE_END\n")
    fifo = case / "control.fifo"
    command = [str(args.hatari.resolve()), "--machine", "falcon", "--monitor", "rgb",
               "--memsize", "14", "--cpuclock", str(args.cpuclock), "--fpu", "68882",
               "--dsp", "emu", "--cpu-exact", "on", "--compatible", "on",
               "--tos", str(args.tos.resolve()), "--harddrive", str(hd),
               "--fast-boot", "on", "--fast-forward", "on", "--sound", "49170",
               "--confirm-quit", "off", "--conout", "2", "--cmd-fifo", str(fifo),
               "--run-vbls", str((args.delay_ms + args.duration_ms) // 10 + 6000),
               "--parse", "prg:" + str(case / "start.ini"), "--auto", "C:\\SCUMMVM\\PCMTEST.PRG"]
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    with (case / "hatari.log").open("w") as log:
        proc = subprocess.Popen(command, cwd=case, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            # Close each writer after sending so Hatari sees EOF between
            # commands instead of repeatedly logging nonblocking EAGAIN.
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
            def send(command):
                # Never block opening the FIFO if Hatari exits between polls.
                with os.fdopen(os.open(fifo, os.O_WRONLY | os.O_NONBLOCK), "w") as control:
                    control.write(command + "\n")
            send(f"hatari-path soundout {case / 'output.wav'}")
            time.sleep(0.1)
            send("hatari-shortcut recsound")
            deadline = time.monotonic() + 600
            while proc.poll() is None:
                report = app / "PCMSTAT.TXT"
                if report.exists() and "probe_report_complete=1" in report.read_text(errors="replace"):
                    time.sleep(0.5)
                    send("hatari-shortcut quit")
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError("Hatari timeout")
                time.sleep(0.2)
            proc.wait(timeout=15)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            fifo.unlink(missing_ok=True)
    report = app / "PCMSTAT.TXT"
    if not report.exists():
        raise SystemExit(f"No probe report; inspect {case / 'hatari.log'}")
    text = report.read_text()
    stats = {key: int(value) for key, value in re.findall(r"^(\w+)=(\d+)$", text, re.M)}
    if not stats.get("probe_report_complete") or not stats.get("start_ms") or not stats.get("end_ms") or not (case / "cpu.txt").exists():
        raise SystemExit(f"Probe did not complete its measurement; inspect {case / 'hatari.log'}")
    cpu = (case / "cpu.txt").read_text()
    stats["cpu_hz"] = int(re.search(r"Cycles/second:\s*(\d+)", cpu)[1])
    expected_hz = {16: 16042494, 32: 32084988}[args.cpuclock]
    if stats["cpu_hz"] != expected_hz:
        raise SystemExit(f"Uncalibrated CPU clock: {stats['cpu_hz']}")
    stats["source_sha256"] = hashlib.sha256(args.pcm.read_bytes()).hexdigest()
    stats["source_kind"] = "FCM1 live Xylophone diagnostic" if args.live_fcm else "prerendered PCM"
    stats["binary_sha256"] = hashlib.sha256((app / "PCMTEST.PRG").read_bytes()).hexdigest()
    stats["speech_requested"] = args.speech
    stats["click"] = args.click
    stats["samples_per_half"] = args.samples
    stats["clock_resolution_ms"] = 5
    stats["storage"] = "Hatari GEMDOS host directory; no physical disk timing claim"
    stats["command"] = command
    (case / "result.json").write_text(json.dumps(stats, indent=2) + "\n")
    print(json.dumps(stats, indent=2))


if __name__ == "__main__":
    main()
