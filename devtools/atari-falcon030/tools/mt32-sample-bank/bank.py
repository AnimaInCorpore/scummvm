#!/usr/bin/env python3
"""Build an experimental dry mono bank and auditable comparisons. Requires NumPy.

This is an approximation, not an MT-32 emulator. WAV headers round the Falcon's
24584.9609375 Hz rate to 24585; the bank and timing model use the exact rate.
"""
import argparse
import html
import json
from pathlib import Path
import wave
import numpy as np

RATE = 25175000 / 1024


def wav(path, samples):
    with wave.open(str(path), "wb") as f:
        f.setnchannels(1 if samples.ndim == 1 else samples.shape[1])
        f.setsampwidth(2)
        f.setframerate(round(RATE))
        f.writeframes(np.clip(np.rint(samples), -32768, 32767).astype("<i2").tobytes())


def resample(x):
    # Fourier lowpass/resampling. Zero padding separates the periodic boundary
    # from the note; captures end with four seconds of release, normally silence.
    padded = np.pad(x, (4096, 4096))
    n, m = len(padded), round(len(padded) * RATE / 32000)
    spectrum = np.fft.rfft(padded)[:m // 2 + 1].copy()
    if m % 2 == 0:
        spectrum[-1] = 2 * spectrum[-1].real
    out = np.fft.irfft(spectrum, n=m) * m / n
    start = round(4096 * RATE / 32000)
    return out[start:start + round(len(x) * RATE / 32000)]


def choose_loop(x):
    # Search genuinely similar windows, retaining 0.4..1.6 seconds per loop.
    # A 20 ms baked crossfade costs no extra runtime voice. Loop start advances
    # past the incoming window because that window is already in the crossfade.
    width = round(.020 * RATE)
    best = None
    for start_sec in (.25, .5, .75, 1., 1.5, 2.):
        start = round(start_sec * RATE)
        first, last = start + round(.4 * RATE), start + round(1.6 * RATE)
        windows = np.lib.stride_tricks.sliding_window_view(x[first:last + width], width)[::32]
        a = x[start:start + width]
        error = np.mean((windows - a) ** 2, axis=1)
        energy = np.mean(windows ** 2, axis=1) + np.mean(a*a) + 1
        scores = error / energy
        i = int(np.argmin(scores))
        end = first + 32 * i
        candidate = (float(scores[i]), start, end)
        if best is None or candidate < best:
            best = candidate
    score, start, end = best
    bank = x[:end + width].copy()
    t = np.linspace(0, 1, width)
    bank[end:end + width] = (1-t)*x[end:end + width] + t*x[start:start + width]
    return np.rint(bank).clip(-32768, 32767).astype(np.int16), start + width, score


def envelope(x, block):
    n = len(x) // block * block
    return np.sqrt(np.mean(x[:n].reshape(-1, block)**2, axis=1) + 1e-12)


def release_curve(x, hold):
    block = round(RATE / 200)
    level = np.sqrt(np.mean(x[hold-round(.2*RATE):hold] ** 2) + 1)
    curve = envelope(x[hold:], block) / level
    return np.r_[1., np.minimum(curve, 1.), 0.]


def replay(data, loop, curve, hold, total, step=1., gain=1.):
    p = np.arange(total) * step
    p = np.where(p < len(data), p, loop + (p-len(data)) % (len(data)-loop))
    i = np.floor(p).astype(np.int64)
    j = np.where(i+1 < len(data), i+1, loop)
    out = data[i].astype(float) * (1-(p-i)) + data[j] * (p-i)
    if total > hold:
        out[hold:] *= np.interp(np.arange(total-hold) * 200 / RATE,
                                np.arange(len(curve)), curve)
    return out * gain


def spectral_error(reference, actual):
    def spectrum(x):
        frames = np.lib.stride_tricks.sliding_window_view(x, 1024)[::512]
        return abs(np.fft.rfft(frames * np.hanning(1024), axis=1))
    a, b = spectrum(reference), spectrum(actual)
    floor = max(float(a.max()) * 1e-3, 1)
    mask = a > floor
    db = 20*np.log10(np.maximum(b, floor) / np.maximum(a, floor))
    return float(np.sqrt(np.mean(db[mask]**2)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--captures", type=Path, default=Path("build/captures"))
    parser.add_argument("--output", type=Path, default=Path("build/bank"))
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    records, bank, arrays, source = [], [], {}, {}
    for line in (args.captures / "captures.tsv").read_text().splitlines():
        p, k, v, h, validation, file, name = line.split("\t")
        rec = dict(program=int(p), key=int(k), velocity=int(v),
                   hold=round(int(h)*RATE/32000), validation=bool(int(validation)),
                   file=file, name=name)
        rec["id"] = file.removesuffix(".s16le")
        records.append(rec)
        x = resample(np.fromfile(args.captures / file, dtype="<i2").astype(float))
        source[rec["id"]] = x
        if rec["validation"]:
            continue
        data, loop, score = choose_loop(x)
        curve = release_curve(x, rec["hold"])
        entry = {k: rec[k] for k in ("id", "name", "program", "key", "velocity")}
        entry.update(frames=len(data), loop_start=loop, bytes=2*len(data),
                     loop_match_error=score, release_curve=curve.tolist())
        bank.append(entry)
        arrays[rec["id"]] = data
        data.astype("<i2").tofile(out / (rec["id"] + ".s16le"))
    comparisons, listening_rows = [], []
    for rec in records:
        x = source[rec["id"]]
        candidates = [b for b in bank if b["program"] == rec["program"]]
        b = min(candidates, key=lambda b: (abs(b["key"]-rec["key"]),
                                           abs(b["velocity"]-rec["velocity"])))
        gain = rec["velocity"] / b["velocity"]
        y = replay(arrays[b["id"]], b["loop_start"], b["release_curve"],
                   rec["hold"], len(x), 2**((rec["key"]-b["key"])/12), gain)
        energy_x, energy_y = envelope(x, 1024), envelope(y, 1024)
        mask = energy_x > max(float(energy_x.max()) * .01, 1)
        envelope_db = 20*np.log10(np.maximum(energy_y[mask], 1) / energy_x[mask])
        report = dict(id=rec["id"], source=b["id"], held_out=rec["validation"],
                      spectral_error_db=spectral_error(x, y),
                      envelope_error_db=float(np.sqrt(np.mean(envelope_db**2))))
        comparisons.append(report)
        if rec["validation"] or (rec["key"] == 60 and rec["velocity"] == 100):
            # Identical level, no per-file normalization. L=reference R=sampled.
            wav(out / (rec["id"] + "-reference.wav"), x)
            wav(out / (rec["id"] + "-sampled.wav"), y)
            wav(out / (rec["id"] + "-compare-LR.wav"), np.column_stack((x, y)))
            label = html.escape(f"{rec['name']} · key {rec['key']} · velocity {rec['velocity']} · " +
                                ("350 ms note" if "early" in rec["id"] else "8 second note"))
            listening_rows.append(f'<tr><th>{label}</th><td><audio controls preload="none" '
                                  f'src="{rec["id"]}-reference.wav"></audio></td><td>'
                                  f'<audio controls preload="none" src="{rec["id"]}-sampled.wav"></audio></td></tr>')
    manifest = dict(rate=RATE, format="signed 16-bit mono little endian", entries=bank,
                    pcm_bytes=sum(b["bytes"] for b in bank),
                    comparisons=comparisons,
                    limitations=["three key zones and two velocity layers only",
                                 "release derived from long-held reference",
                                 "no live filter, modulation, reverb or SysEx",
                                 "spectral error measures difference, not listening quality"])
    (out / "bank.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (out / "listen.html").write_text('''<!doctype html><meta charset="utf-8">
<title>MT-32 sample bank comparisons</title>
<style>body{font:17px system-ui;max-width:1100px;margin:40px auto;padding:0 20px;color:#172434;background:#f5f7fa}
table{width:100%;border-collapse:collapse}th,td{padding:16px 10px;border-bottom:1px solid #cad2dc;text-align:left}
audio{width:280px}h1{font-size:30px}p{max-width:850px;line-height:1.5}</style>
<h1>Listen to the sample bank experiment</h1>
<p>Left column: dry Munt reference. Right column: recorded attack, looped sustain and an estimated release.
Levels match; there is no per-file loudness normalization. These comparisons use the 16-bit bank,
before the separate 12-bit transport experiment. No reverb is included.</p>
<p>Listen for repeated movement in long strings and Fantasy notes, changed horn tone at key 67 / velocity 80,
and the release after a short note. Measurements establish differences, not a listening-quality verdict.</p>
<table><thead><tr><th>Instrument and note</th><th>Reference</th><th>Sample bank</th></tr></thead><tbody>'''
                                  + "\n".join(listening_rows) + '</tbody></table>\n')
    print(f"{len(bank)} entries, {manifest['pcm_bytes']:,} PCM bytes, {RATE:.8f} Hz")
    for c in comparisons:
        if c["held_out"] or "k060-v100" in c["id"]:
            print(f"{c['id']}: spectrum {c['spectral_error_db']:.2f} dB, envelope {c['envelope_error_db']:.2f} dB")


if __name__ == "__main__":
    main()
