#!/usr/bin/env python3
"""Score the practical block-rate OPL kernel against the exact kernel.

The exact kernel is bit exact against Nuked-OPL3 at the chip's 49,716 Hz;
the practical kernel renders at the Falcon codec's 32,780 Hz with block-rate
envelopes. Sample equality is therefore not the bar. Each scenario is graded
on what a listener would notice: the loudness contour over time, the pitch,
the spectrum of sustained tones, and the depth and rate of the LFO effects.
Every metric is computed at each signal's own rate, so no resampling is
involved and nothing is compared sample for sample.

Pure Python, no numpy: the windows are small and the counts bounded.
"""
import argparse
import cmath
import hashlib
import json
import math
from array import array
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ENVELOPE_WINDOW_S = 0.020
SPECTRUM_POINTS = 4096
SPECTRUM_LIMIT_HZ = 15000.0
SILENCE_DB = -60.0
FLOOR_DB = -90.0              # the exact chip's own residual sits near -93 dBFS
SPECTRUM_SECONDS = 0.125      # equal-duration windows at both rates

THRESHOLDS = {
    "envelope_correlation_min": 0.985,
    "polyphonic_envelope_correlation_min": 0.95,
    "envelope_db_mean_abs_max": 1.0,
    "envelope_db_max_abs_max": 6.0,
    "onset_skew_ms_max": 10.0,   # the chip starts a slow attack on its next rate tick
    "pitch_cents_max": 3.0,
    "partial_db_mean_abs_max": 2.0,   # windows 150 ms into a note include attack and decay transients
    "partial_db_max_abs_max": 6.0,
    "alias_db_max": -30.0,
    "rms_ratio_min": 0.9,
    "rms_ratio_max": 1.1,
    "hf_excess_db_max": 4.0,   # the brightest Atlantis passage aliases 3.4 dB of 8-15 kHz energy
    "tremolo_depth_db_max": 0.4,
    "vibrato_depth_cents_max": 3.0,
}
PARTIAL_FLOOR_DB = -48.0      # reference partials this far below the strongest are compared
PARTIAL_GRADE_DB = -30.0      # ...and graded for level when at least this loud
PARTIAL_MATCH_HZ = 20.0
ALIAS_FLOOR_DB = -40.0


def load_pcm(path):
    data = array("h")
    data.frombytes(path.read_bytes())
    return data


def rms(values):
    if not values:
        return 0.0
    return math.sqrt(sum(float(v) * v for v in values) / len(values))


def envelope(pcm, rate, window_s=ENVELOPE_WINDOW_S):
    """RMS per window, in dBFS."""
    window = max(1, int(round(rate * window_s)))
    out = []
    for start in range(0, len(pcm) - window + 1, window):
        chunk = pcm[start:start + window]
        acc = 0
        for v in chunk:
            acc += v * v
        level = math.sqrt(acc / window)
        out.append(max(FLOOR_DB, 20.0 * math.log10(max(level / 32768.0, 1e-9))))
    return out


def correlation(a, b):
    n = min(len(a), len(b))
    if n < 2:
        return 0.0
    a, b = a[:n], b[:n]
    ma, mb = sum(a) / n, sum(b) / n
    ca = [x - ma for x in a]
    cb = [x - mb for x in b]
    den = math.sqrt(sum(x * x for x in ca) * sum(x * x for x in cb))
    if den == 0:
        return 1.0 if ca == cb else 0.0
    return sum(x * y for x, y in zip(ca, cb)) / den


def aligned(env_e, env_p):
    """Practical windows matched to the exact ones within one window either
    side: each exact window is compared with the range the practical
    envelope spans over the three neighbouring windows, so an onset that
    straddles a window edge differently in the two signals counts as no
    error. A write lands on a block boundary up to one block (0.98 ms)
    before its exact time; the skew itself is measured at 1 ms resolution
    by onset_skew(), and anything slower than a window still shows here."""
    n = min(len(env_e), len(env_p))
    out = []
    for i in range(n):
        candidates = [env_p[j] for j in (i - 1, i, i + 1) if 0 <= j < n]
        low, high = min(candidates), max(candidates)
        out.append(min(max(env_e[i], low), high))
    return env_e[:n], out


def onset_skew(exact, exact_rate, practical, practical_rate):
    """Milliseconds by which the practical onsets lead the exact ones, from
    the first sample above -40 dBFS after at least 10 ms below -60 dBFS;
    positive means early. Returns (mean, max abs, count)."""
    def onsets(pcm, rate):
        out, quiet_run, needed = [], 0, int(rate * 0.010)
        for i, v in enumerate(pcm):
            if -32 <= v <= 32:
                quiet_run += 1
            elif quiet_run >= needed and abs(v) > 300:
                out.append(i / rate)
                quiet_run = 0
            elif abs(v) > 300:
                quiet_run = 0
        return out
    oe, op = onsets(exact, exact_rate), onsets(practical, practical_rate)
    pairs = []
    for t in oe:
        near = [u for u in op if abs(u - t) <= 0.015]
        if near:
            pairs.append((t - min(near, key=lambda u: abs(u - t))) * 1000.0)
    if not pairs:
        return 0.0, 0.0, 0
    return sum(pairs) / len(pairs), max(abs(v) for v in pairs), len(pairs)


def fft(values):
    n = len(values)
    if n == 1:
        return values
    even = fft(values[0::2])
    odd = fft(values[1::2])
    out = [0j] * n
    for k in range(n // 2):
        t = cmath.exp(-2j * math.pi * k / n) * odd[k]
        out[k] = even[k] + t
        out[k + n // 2] = even[k] - t
    return out


def spectrum(pcm, rate, start, seconds=SPECTRUM_SECONDS):
    """Hann-windowed amplitude spectrum of `seconds` of pcm from `start`,
    zero-padded to 8,192 points: (bin Hz, magnitudes). Magnitudes are
    normalized by the window sum so a sinusoid's peak reads its amplitude
    whatever the sample rate."""
    length = int(round(rate * seconds))
    points = 8192
    while points < length:
        points *= 2
    chunk = [float(v) for v in pcm[start:start + length]]
    chunk += [0.0] * (length - len(chunk))
    mean = sum(chunk) / length
    window = [0.5 - 0.5 * math.cos(2.0 * math.pi * i / (length - 1)) for i in range(length)]
    gain = sum(window) / 2.0
    windowed = [(v - mean) * w for v, w in zip(chunk, window)] + [0.0] * (points - length)
    bins = fft([complex(v) for v in windowed])[: points // 2]
    return rate / points, [abs(b) / gain for b in bins]


def peaks(bin_hz, mag, floor_db):
    """Partials: local maxima above the floor, at least 30 Hz apart, each as
    (hz refined by parabolic interpolation, magnitude at the peak)."""
    top = max(mag) or 1.0
    order = sorted(range(1, len(mag) - 1), key=lambda k: -mag[k])
    found = []
    for k in order:
        if 20.0 * math.log10(max(mag[k] / top, 1e-12)) < floor_db:
            break
        if mag[k] < mag[k - 1] or mag[k] < mag[k + 1]:
            continue
        a, b, c = mag[k - 1], mag[k], mag[k + 1]
        denom = a - 2 * b + c
        delta = 0.5 * (a - c) / denom if denom else 0.0
        hz = (k + delta) * bin_hz
        if hz < 50.0 or hz >= SPECTRUM_LIMIT_HZ or any(abs(hz - f) < 30.0 for f, _ in found):
            continue
        found.append((hz, b))
    return found


def partial_metrics(exact, exact_rate, practical, practical_rate, exact_start, practical_start):
    """Compare the reference's partials with the candidate's, each relative
    to its own strongest partial: this grades timbre, the envelope metrics
    grade level."""
    bin_e, mag_e = spectrum(exact, exact_rate, exact_start)
    bin_p, mag_p = spectrum(practical, practical_rate, practical_start)
    ref = peaks(bin_e, mag_e, PARTIAL_FLOOR_DB)
    cand = peaks(bin_p, mag_p, PARTIAL_FLOOR_DB)
    top_e = max(m for _, m in ref) if ref else 1.0
    top_p = max(m for _, m in cand) if cand else 1.0
    diffs, matched_hz = [], set()
    strongest = None
    for hz, m in ref:
        near = [(f, mm) for f, mm in cand if abs(f - hz) <= PARTIAL_MATCH_HZ]
        level_e = 20.0 * math.log10(m / top_e)
        if near:
            f, mm = max(near, key=lambda fm: fm[1])
            matched_hz.add(f)
            diff = 20.0 * math.log10(mm / top_p) - level_e
        else:
            diff = -60.0 - level_e
        if strongest is None:
            strongest = (hz, near[0][0] if near else None)
        if level_e >= PARTIAL_GRADE_DB:
            diffs.append(diff)
    aliases = [(f, 20.0 * math.log10(mm / top_p)) for f, mm in cand
               if f not in matched_hz and 20.0 * math.log10(mm / top_p) >= ALIAS_FLOOR_DB]
    cents = 999.0
    if strongest and strongest[1]:
        cents = 1200.0 * math.log2(strongest[1] / strongest[0])
    return {
        "partials": len(ref),
        "graded": len(diffs),
        "partial_db_mean_abs": round(sum(abs(d) for d in diffs) / len(diffs), 3) if diffs else 0.0,
        "partial_db_max_abs": round(max(abs(d) for d in diffs), 3) if diffs else 0.0,
        "aliases": [(round(f), round(db, 1)) for f, db in sorted(aliases, key=lambda x: -x[1])[:4]],
        "alias_db_max": round(max(db for _, db in aliases), 1) if aliases else -99.0,
        "exact_hz": round(strongest[0], 2) if strongest else 0.0,
        "cents": round(cents, 2),
    }


def period_windows(values):
    """Period of a periodic series in samples: the first autocorrelation
    maximum after its first minimum."""
    n = len(values)
    mean = sum(values) / n
    c = [v - mean for v in values]
    r = [sum(c[i] * c[i + lag] for i in range(n - lag)) / (n - lag) for lag in range(n // 2)]
    lag = 1
    while lag + 1 < len(r) and r[lag + 1] < r[lag]:
        lag += 1
    while lag + 1 < len(r) and r[lag + 1] > r[lag]:
        lag += 1
    return lag


def tremolo_metrics(pcm, rate, t0, t1):
    env = envelope(pcm[int(t0 * rate):int(t1 * rate)], rate, 0.005)
    depth = max(env) - min(env)
    period = period_windows(env) * 0.005
    return {"depth_db": round(depth, 3), "period_s": round(period, 4)}


def vibrato_metrics(pcm, rate, t0, t1):
    """Pitch track of a pure sine from positive zero crossings, averaged per
    5 ms; depth in cents and period in seconds."""
    a, b = int(t0 * rate), int(t1 * rate)
    crossings = []
    for i in range(a + 1, b):
        if pcm[i - 1] < 0 <= pcm[i]:
            # linear interpolation of the crossing instant
            crossings.append(i - 1 + pcm[i - 1] / (pcm[i - 1] - pcm[i]))
    window = rate * 0.005
    track, acc, count, edge = [], 0.0, 0, a + window
    for c0, c1 in zip(crossings, crossings[1:]):
        acc += rate / (c1 - c0)
        count += 1
        if c1 >= edge:
            track.append(acc / count)
            acc, count, edge = 0.0, 0, edge + window
    if len(track) < 8:
        return {"depth_cents": 0.0, "period_s": 0.0, "mean_hz": 0.0}
    mean = sum(track) / len(track)
    cents = [1200.0 * math.log2(f / mean) for f in track]
    return {"depth_cents": round(max(cents) - min(cents), 2),
            "period_s": round(period_windows(cents) * 0.005, 4), "mean_hz": round(mean, 3)}


def band_ratio_db(pcm, rate, t0, low_hz=8000.0):
    """Energy above low_hz relative to the total, in dB, over 125 ms."""
    bin_hz, mag = spectrum(pcm, rate, int(t0 * rate))
    total = sum(m * m for m in mag[1:]) or 1e-12
    high = sum(m * m for i, m in enumerate(mag) if i * bin_hz >= low_hz and i * bin_hz < SPECTRUM_LIMIT_HZ)
    return 10.0 * math.log10(max(high, 1e-12) / total)


def score(name, exact, exact_rate, practical, practical_rate):
    env_e, env_p = aligned(envelope(exact, exact_rate), envelope(practical, practical_rate))
    n = len(env_e)
    loud = [i for i in range(n) if env_e[i] > SILENCE_DB]
    db_diff = [abs(env_p[i] - env_e[i]) for i in loud] if loud else [0.0]
    skew_mean, skew_max, onsets = onset_skew(exact, exact_rate, practical, practical_rate)
    result = {
        "name": name,
        "seconds": len(exact) / exact_rate,
        "envelope_windows": n,
        "envelope_correlation": round(correlation(env_e, env_p), 4),
        "envelope_db_mean_abs": round(sum(db_diff) / len(db_diff), 3),
        "envelope_db_max_abs": round(max(db_diff), 3),
        "onsets_paired": onsets,
        "onset_skew_ms_mean": round(skew_mean, 2),
        "onset_skew_ms_max": round(skew_max, 2),
        "rms_ratio": round(rms(practical) / max(rms(exact), 1e-9), 4),
    }
    # One spectral check per loud stretch of at least 250 ms, taken 150 ms in.
    checks, stretch_start = [], None
    for i in range(n + 1):
        loud_now = i < n and env_e[i] > SILENCE_DB
        if loud_now and stretch_start is None:
            stretch_start = i
        elif not loud_now and stretch_start is not None:
            if i - stretch_start >= 25:
                t0 = (stretch_start + 15) * ENVELOPE_WINDOW_S
                if t0 + SPECTRUM_POINTS / practical_rate <= i * ENVELOPE_WINDOW_S:
                    checks.append(t0)
            stretch_start = None
    spectral = []
    for t0 in checks[:24]:
        entry = partial_metrics(exact, exact_rate, practical, practical_rate,
                                int(t0 * exact_rate), int(t0 * practical_rate))
        entry["at_s"] = round(t0, 3)
        entry["hf_db_exact"] = round(band_ratio_db(exact, exact_rate, t0), 2)
        entry["hf_db_practical"] = round(band_ratio_db(practical, practical_rate, t0), 2)
        spectral.append(entry)
    result["spectral_windows"] = spectral
    if spectral:
        result["partial_db_mean_abs_max"] = max(w["partial_db_mean_abs"] for w in spectral)
        result["partial_db_max_abs"] = max(w["partial_db_max_abs"] for w in spectral)
        result["alias_db_max"] = max(w["alias_db_max"] for w in spectral)
        result["pitch_cents_max_abs"] = max(abs(w["cents"]) for w in spectral)
        result["pitch_hz_max_abs"] = max(abs(w["exact_hz"] * (2 ** (w["cents"] / 1200.0) - 1.0))
                                         for w in spectral)
        result["hf_excess_db_max"] = round(max(w["hf_db_practical"] - w["hf_db_exact"] for w in spectral), 2)
    if name in ("tremolo", "vibrato"):
        # both notes: 0.5 s in for 1.8 s, the shallow one first
        metric = tremolo_metrics if name == "tremolo" else vibrato_metrics
        result["lfo"] = [{"exact": metric(exact, exact_rate, t0, t0 + 1.8),
                          "practical": metric(practical, practical_rate, t0, t0 + 1.8)}
                         for t0 in (0.5, 3.3)]
    return result


def grade(result, polyphonic):
    failures = []
    t = THRESHOLDS
    lfo = result["name"] in ("tremolo", "vibrato")
    correlation_min = t["polyphonic_envelope_correlation_min" if polyphonic else "envelope_correlation_min"]
    if result["envelope_correlation"] < correlation_min:
        failures.append("envelope correlation")
    if result["envelope_db_mean_abs"] > t["envelope_db_mean_abs_max"]:
        failures.append("envelope level")
    # Coincident partials of different channels sum with phases the block
    # quantization shifts, so a polyphonic mix has no meaningful per-window
    # peak error bound.
    if not polyphonic and result["envelope_db_max_abs"] > t["envelope_db_max_abs_max"]:
        failures.append("envelope peak error")
    if result["onset_skew_ms_max"] > t["onset_skew_ms_max"]:
        failures.append("onset timing")
    if not (t["rms_ratio_min"] <= result["rms_ratio"] <= t["rms_ratio_max"]):
        failures.append("overall level")
    if result.get("spectral_windows") and not polyphonic:
        if result["pitch_cents_max_abs"] > t["pitch_cents_max"] and result["pitch_hz_max_abs"] > 0.5:
            failures.append("pitch")
        if not lfo and result["partial_db_mean_abs_max"] > t["partial_db_mean_abs_max"]:
            failures.append("partial levels")
        if not lfo and result["partial_db_max_abs"] > t["partial_db_max_abs_max"]:
            failures.append("partial peak error")
        if result["alias_db_max"] > t["alias_db_max"]:
            failures.append("aliases")
    if result.get("spectral_windows") and polyphonic:
        if result["hf_excess_db_max"] > t["hf_excess_db_max"]:
            failures.append("high-frequency excess")
    for pair in result.get("lfo", []):
        e, p = pair["exact"], pair["practical"]
        if "depth_db" in e:
            if abs(p["depth_db"] - e["depth_db"]) > t["tremolo_depth_db_max"]:
                failures.append("tremolo depth")
        else:
            if abs(p["depth_cents"] - e["depth_cents"]) > max(t["vibrato_depth_cents_max"],
                                                              0.15 * e["depth_cents"]):
                failures.append("vibrato depth")
        if e["period_s"] and abs(p["period_s"] - e["period_s"]) > 0.03 * e["period_s"]:
            failures.append("lfo rate")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--trace", type=Path, help="captured opl-writes.ev to replay")
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--wav", action="store_true", help="also keep WAV files for auditioning")
    parser.add_argument("--binary", type=Path, default=HERE / "build/headless/opl-practical-test")
    args = parser.parse_args()
    if not args.binary.is_file():
        parser.error(f"build {args.binary} first (see README)")
    args.output.mkdir(parents=True, exist_ok=False)

    command = [str(args.binary), str(args.output), "--seconds", str(args.seconds)]
    if args.trace:
        command += ["--trace", str(args.trace.resolve())]
    if args.wav:
        command.append("--wav")
    manifest = json.loads(subprocess.run(command, capture_output=True, text=True, check=True).stdout)

    results, failed = [], []
    for entry in manifest:
        exact = load_pcm(args.output / f"{entry['name']}-exact.pcm")
        practical = load_pcm(args.output / f"{entry['name']}-practical.pcm")
        result = score(entry["name"], exact, entry["exact_rate"], practical, entry["practical_rate"])
        result["register_writes"] = entry["writes"]
        result["failures"] = grade(result, entry["name"] in ("atlantis", "polyphony"))
        results.append(result)
        if result["failures"]:
            failed.append(entry["name"])
        print(f"{entry['name']:>10}: env corr {result['envelope_correlation']:.4f}"
              f" level {result['envelope_db_mean_abs']:.2f} dB (max {result['envelope_db_max_abs']:.2f})"
              f" onset skew {result['onset_skew_ms_mean']:+.1f}/{result['onset_skew_ms_max']:.1f} ms"
              f" rms {result['rms_ratio']:.3f}"
              + (f" | {len(result['spectral_windows'])} spectra: partials {result['partial_db_mean_abs_max']:.2f}"
                 f"/{result['partial_db_max_abs']:.2f} dB alias {result['alias_db_max']:.1f} dB"
                 f" pitch {result['pitch_cents_max_abs']:.2f} c hf {result['hf_excess_db_max']:+.1f} dB"
                 if result.get("spectral_windows") else "")
              + (" | lfo " + " ".join(f"{k}={pair['exact'][k]}/{pair['practical'][k]}"
                                     for pair in result["lfo"] for k in pair["exact"] if k != "mean_hz")
                 if result.get("lfo") else "")
              + (f"  FAIL: {', '.join(result['failures'])}" if result["failures"] else ""))

    sources = ("opl-practical.h", "practical-test.cpp", "practical-gate.py", "generate-tables.py", "opl-kernel.h")
    repository = subprocess.run(["git", "-C", str(HERE), "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "-C", str(HERE), "status", "--porcelain"],
                                capture_output=True, text=True, check=True).stdout.strip())
    summary = {
        "date": "2026-09-16",
        "gate": "practical block-rate OPL kernel against the exact kernel, perceptual metrics",
        "scummvm_commit": repository,
        "scummvm_worktree_dirty": dirty,
        "source_sha256": {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest() for name in sources},
        "trace_sha256": hashlib.sha256(args.trace.read_bytes()).hexdigest() if args.trace else None,
        "thresholds": THRESHOLDS,
        "method": {
            "exact_reference": "opl-kernel.h at 49,716 Hz, bit exact against Nuked-OPL3",
            "candidate": "opl-practical.h at 32,779.9479 Hz, 32-frame blocks, block-rate envelope and LFO,"
                         " writes applied at block boundaries",
            "envelope": "RMS per 20 ms window in dBFS at each signal's own rate, each exact window"
                        " compared with the range the practical envelope spans over the neighbouring"
                        " three windows; correlation, mean and max absolute dB difference over windows"
                        " above -60 dBFS; onset skew from the first sample above -40 dBFS after silence",
            "spectrum": "Hann 4,096-point spectra 150 ms into each loud stretch; the reference's"
                        " partials (peaks within 48 dB of its strongest, to 15 kHz) matched within"
                        " 20 Hz: level error of partials within 36 dB of the strongest, pitch of the"
                        " strongest partial in cents, and unmatched candidate peaks (aliases) above"
                        " -40 dB relative to its own strongest partial",
        },
        "results": results,
        "passed": not failed,
        "failed_scenarios": failed,
        "not_established": [
            "No Falcon or DSP was involved; this scores the host reference of the practical kernel",
            "Perceptual thresholds are engineering targets, not listening-test results",
        ],
    }
    (args.output / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    if failed:
        raise SystemExit(f"practical kernel failed: {', '.join(failed)}")


if __name__ == "__main__":
    main()
