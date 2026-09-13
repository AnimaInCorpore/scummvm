#!/usr/bin/env python3
"""Derive one-cycle, band-limited resident tables and sustain comparisons.

This deliberately removes detuned partial motion: it is a wavetable substitute,
not a transparent compression of an MT-32 note. Requires NumPy.
"""
import argparse
import html
import json
from pathlib import Path
import numpy as np
from bank import RATE, resample, spectral_error, wav


def make_table(x, key, size):
    frequency = 440 * 2**((key-69)/12)
    # Fit harmonics at the requested pitch over a 100 ms Hann window. This
    # averages detuned components into a static spectrum. Harmonics above the
    # output Nyquist are omitted before table construction.
    start = round(2*RATE)
    n = round(.100*RATE)
    segment = x[start:start+n]
    t = np.arange(n) / RATE
    window = np.hanning(n)
    count = min(size//2-1, int(.47*RATE/frequency))
    harmonics = np.arange(1, count+1)
    coefficients = (np.exp(-2j*np.pi*harmonics[:, None]*frequency*t) @ (segment*window)) * 2/window.sum()
    phases = np.arange(size) / size
    table = np.real(coefficients @ np.exp(2j*np.pi*harmonics[:, None]*phases))
    # Preserve the local reference RMS, not a common arbitrary loudness.
    table *= np.sqrt(np.mean(segment**2) / max(np.mean(table**2), 1))
    if np.max(np.abs(table)) > 32767:
        raise ValueError("Resident table clips")
    return np.rint(table).astype(np.int16), start, frequency


def oscillator(table, step, count, phase=0, linear=False):
    p = ((phase + np.arange(count, dtype=np.int64)*step) & 65535) * len(table)
    index, frac = p >> 16, (p & 65535)/65536
    y = table[index].astype(float)
    if linear:
        y += (table[(index+1) % len(table)].astype(float)-y)*frac
    return y


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--captures", type=Path, default=Path("build/captures"))
    parser.add_argument("--output", type=Path, default=Path("build/resident-bank"))
    parser.add_argument("--size", type=int, choices=(512, 1024), default=1024)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    entries, comparisons, rows = [], [], []
    for line in (args.captures / "captures.tsv").read_text().splitlines():
        p, k, v, h, heldout, file, name = line.split("\t")
        if int(heldout):
            continue
        x = resample(np.fromfile(args.captures/file, dtype="<i2").astype(float))
        table, start, frequency = make_table(x, int(k), args.size)
        ident = file.removesuffix(".s16le")
        table.astype("<i2").tofile(args.output/(ident+".s16le"))
        step = round(frequency*65536/RATE)
        actual_frequency = step*RATE/65536
        entry = dict(id=ident, program=int(p), key=int(k), velocity=int(v), name=name,
                     frames=len(table), step=step, frequency=frequency,
                     actual_frequency=actual_frequency,
                     tuning_error_cents=1200*np.log2(actual_frequency/frequency))
        entries.append(entry)
        n = round(4*RATE)
        reference = x[start:start+n]
        nearest = oscillator(table, step, n)
        linear = oscillator(table, step, n, linear=True)
        error = np.mean((nearest-linear)**2)
        comparison = dict(id=ident, spectral_error_db=spectral_error(reference, nearest),
                          nearest_vs_linear_snr_db=float(10*np.log10(np.mean(linear**2)/max(error, 1e-20))))
        comparisons.append(comparison)
        if int(k)==60 and int(v)==100:
            for suffix, data in (("reference", reference), ("resident", nearest), ("linear", linear)):
                wav(args.output/(ident+"-"+suffix+".wav"), data)
            cells = ''.join(f'<td><audio controls preload="none" src="{ident}-{suffix}.wav"></audio></td>'
                            for suffix in ("reference", "resident", "linear"))
            rows.append(f'<tr><th>{html.escape(name)}</th>{cells}</tr>')
    report = dict(rate=RATE, table_frames=args.size, table_words=sum(e["frames"] for e in entries),
                  entries=entries, comparisons=comparisons,
                  limitations=["stationary one-cycle sustains; recorded attacks are excluded",
                               "16-bit phase increment; nearest-neighbor lookup in DSP",
                               "three key zones and two velocity layers; no listening verdict"])
    (args.output/"bank.json").write_text(json.dumps(report, indent=2)+"\n")
    (args.output/"listen.html").write_text('''<!doctype html><meta charset="utf-8">
<title>Resident sustain comparisons</title><style>body{font:17px system-ui;max-width:1200px;margin:40px auto;padding:20px;background:#f5f7fa;color:#172434}td,th{padding:15px;text-align:left}audio{width:270px}p{max-width:1000px;line-height:1.5}</style>
<h1>Resident sustain experiment</h1><p>Four seconds of dry sustain at key 60, velocity 100.
The reference starts two seconds into a held note. The resident version reduces it to a static, repeating
single-cycle table. The third column uses the same table with offline linear interpolation as a quality control;
it is not the tested DSP algorithm. All use the reference's local RMS level.</p>
<p>Listen for lost beating, chorus and evolving tone. No attack, release or reverb is present.</p>
<table><tr><th>Patch</th><th>Munt reference</th><th>DSP resident algorithm</th><th>Linear control</th></tr>'''
                                         + ''.join(rows) + '</table>\n')
    print(f"{len(entries)} resident tables: {report['table_words']} DSP words")
    for c in comparisons:
        if "k060-v100" in c["id"]:
            print(c)


if __name__ == "__main__":
    main()
