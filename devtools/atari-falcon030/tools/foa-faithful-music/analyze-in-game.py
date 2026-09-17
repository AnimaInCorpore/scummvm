#!/usr/bin/env python3
"""Check probe deadlines and align the recorded stereo music against its source.

Requires NumPy. Hatari's host WAV is resampled, so correlation is a continuity
check, not a sample-exact digital transport claim. The L-R signal cancels
centered mono speech; this check does not validate speech quality itself.
"""
import argparse
from bisect import bisect_right
import json
from pathlib import Path
import wave

import numpy as np

RATE = 25175000 / 512


def correlate(source, recording):
    """Normalized correlation at every complete source placement."""
    source = source.astype(np.float64)
    source -= source.mean()
    recording = recording.astype(np.float64)
    count = len(source)
    size = 1 << (count + len(recording) - 2).bit_length()
    dot = np.fft.irfft(np.fft.rfft(recording, size) *
                       np.conj(np.fft.rfft(source, size)), size)[:len(recording) - count + 1]
    sums = np.r_[0, recording.cumsum()]
    squares = np.r_[0, (recording * recording).cumsum()]
    energy = squares[count:] - squares[:-count] - (sums[count:] - sums[:-count]) ** 2 / count
    return dot / np.sqrt(np.maximum(1, energy * np.dot(source, source)))


def analyze(case, source_path=None):
    stats = json.loads((case / "result.json").read_text())
    report = (case / "HD/SCUMMVM/PCMSTAT.TXT").read_text()
    irq_text = report.split("irq_ms,stopped,update_age_ms\n")[1].split("mix_begin_ms")[0]
    mix_text = report.split("mix_begin_ms,mix_end_ms,consumed_bytes,queued_bytes,speech,frames\n")[1].split("probe_report")[0]
    irqs = [list(map(int, row.split(","))) for row in irq_text.strip().splitlines()]
    mixes = [list(map(int, row.split(","))) for row in mix_text.strip().splitlines()]
    times = [row[0] for row in irqs]
    slack = [times[index] - row[1] for row in mixes
             if (index := bisect_right(times, row[0])) < len(times)]
    elapsed = (stats["end_ms"] - stats["start_ms"]) / 1000
    result = {
        "source_sha256": stats["source_sha256"],
        "binary_sha256": stats["binary_sha256"],
        "elapsed_seconds": elapsed,
        "block_seconds": stats["samples_per_half"] / RATE,
        "mix_time_fraction": stats["mix_ms"] / (elapsed * 1000),
        "consumed_audio_seconds": stats["consumed_bytes"] / (RATE * 4),
        "min_finish_to_next_irq_ms": min(slack) if slack else None,
        "nonpositive_finish_margins": sum(value <= 0 for value in slack),
        "min_ring_seconds": stats["min_queued_bytes"] / (RATE * 4),
        "dma_stops": stats["dma_stops"],
        "missing_bytes": stats["missing_bytes"],
        "speech_mixes": stats["speech_mixes"],
        "clock_resolution_ms": stats["clock_resolution_ms"],
        # Older checkpoints did not read the hardware DMA pointer.
        "dma_active_before_write": stats.get("dma_active_before_write"),
        "dma_active_after_write": stats.get("dma_active_after_write"),
        # Informational: starvation events while the game loaded.
        "stops_before_start": stats.get("stops_before_start"),
    }
    if stats.get("live_synthesis") and source_path is None:
        raise ValueError("Live synthesis needs an independently generated --source PCM reference")
    raw = np.fromfile(source_path if source_path is not None else case / "HD/SCUMMVM/MUSIC.RAW",
                      dtype=">i2").reshape(-1, 2).astype(np.float64)
    source = raw[:, 0] - raw[:, 1]
    with wave.open(str(case / "output.wav")) as wav:
        if wav.getnchannels() != 2 or wav.getsampwidth() != 2 or wav.getframerate() != round(RATE):
            raise ValueError("Expected a 49170 Hz PCM16 stereo capture")
        capture_rate = wav.getframerate()
        # Ignore the long boot/intro, retaining enough pre-roll for alignment.
        first_frame = max(0, wav.getnframes() - round((elapsed + 15) * capture_rate))
        wav.setpos(first_frame)
        pcm = np.frombuffer(wav.readframes(wav.getnframes() - first_frame), dtype="<i2").reshape(-1, 2)
    difference = pcm[:, 0].astype(np.float64) - pcm[:, 1]
    # Use the first second after one second of source pre-roll as the anchor.
    anchor = round(RATE)
    width = round(RATE)
    if len(source) < anchor + width or np.std(source[anchor:anchor + width]) < 8:
        raise ValueError("Source needs non-silent stereo information in its second second")
    def downsample(signal):
        # Average before decimating so an offset that is not divisible by
        # eight still produces a useful coarse peak for broadband sources.
        return signal[:len(signal) // 8 * 8].reshape(-1, 8).mean(axis=1)
    def align(recording):
        coarse = int(np.argmax(correlate(downsample(source[anchor:anchor + width]), downsample(recording)))) * 8
        lower = max(0, coarse - 16)
        upper = min(len(recording), coarse + width + 16)
        scores = correlate(source[anchor:anchor + width], recording[lower:upper])
        return lower + int(np.argmax(scores)) - anchor, float(np.max(scores))
    # Align both channel orders. Swapped channels invert L-R; without this the
    # capture reports hundreds of low windows around a meaningless origin.
    alignments = {"normal": align(difference), "swapped": align(-difference)}
    order = max(alignments, key=lambda key: alignments[key][1])
    origin, result["anchor_correlation"] = alignments[order]
    recording = difference if order == "normal" else -difference
    result["channel_order"] = order
    result["capture_origin_seconds"] = (first_frame + origin) / capture_rate
    # Inspect overlapping half-second windows. Search only near the linear
    # clock mapping: a repeated/skipped block must fail instead of being hidden
    # by re-aligning the reference after every discontinuity.
    width = round(RATE / 2)
    # Quit cuts off DMA immediately. The final two mixed blocks may still be
    # queued when the quit event is consumed; they are not a capture failure.
    available = min(round(elapsed * RATE), stats["consumed_bytes"] // 4) - 2 * stats["samples_per_half"]
    result["shutdown_excluded_frames"] = 2 * stats["samples_per_half"]
    rows = []
    for start in range(0, max(0, available - width), round(RATE / 4)):
        reference = source[np.arange(start, start + width) % len(source)]
        expected = origin + round(start * capture_rate / RATE)
        lower = max(0, expected - 16)
        upper = min(len(recording), expected + width + 16)
        if upper - lower < width:
            rows.append({"source_seconds": start / RATE, "status": "capture_truncated"})
            continue
        if np.std(reference) < 8:
            # Silence or nearly mono source cannot establish alignment. Check
            # that the recording also has little stereo difference here, and
            # report these windows separately from correlation evidence.
            quiet = np.std(recording[max(0, expected):max(0, expected) + width]) < 16
            rows.append({"source_seconds": start / RATE,
                         "status": "quiet_stereo_reference" if quiet else "unexpected_stereo_energy"})
            continue
        scores = correlate(reference, recording[lower:upper])
        best = int(np.argmax(scores))
        rows.append({"source_seconds": start / RATE, "correlation": float(scores[best]),
                     "offset_frames": lower + best - expected})
    measured = [row for row in rows if "correlation" in row]
    result["waveform_checked_until_source_seconds"] = rows[-1]["source_seconds"] + width / RATE if rows else 0
    result["waveform_windows"] = rows
    result["minimum_correlation"] = min((row["correlation"] for row in measured), default=0)
    result["low_correlation_windows"] = sum(row["correlation"] < .98 for row in measured)
    result["quiet_stereo_windows"] = sum(row.get("status") == "quiet_stereo_reference" for row in rows)
    result["invalid_windows"] = len(rows) - len(measured) - result["quiet_stereo_windows"]
    result["capture_saturated_words"] = int(np.count_nonzero((pcm == 32767) | (pcm == -32768)))
    result["passed"] = bool(
        stats.get("probe_report_complete") and not stats["irq_log_overflow"] and not stats["mix_log_overflow"]
        and slack and min(slack) > stats["clock_resolution_ms"]
        and not stats["dma_stops"] and not stats["missing_bytes"]
        and not result["dma_active_before_write"] and not result["dma_active_after_write"]
        and (not stats["speech_requested"] or stats["speech_mixes"] > 0)
        and abs(result["consumed_audio_seconds"] - elapsed) <= 2 * result["block_seconds"]
        and measured and not result["low_correlation_windows"] and not result["invalid_windows"]
        and result["channel_order"] == "normal")
    result["limitations"] = [
        "Host WAV correlation is not a sample-exact comparison or listening judgment",
        "L-R alignment checks stereo music continuity; it does not validate centered mono speech",
        "Timer sampling is 5 ms; finish margins refer to the next observed IRQ",
        "DMA position checks are boundary samples, not a trace of every DMA read",
        "Hatari GEMDOS reads do not model physical disk latency",
        "One scripted scene does not establish whole-game or interactive iMUSE coverage",
    ]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", type=Path)
    parser.add_argument("--source", type=Path, help="Independent PCM reference for a live synthesis run")
    args = parser.parse_args()
    result = analyze(args.case, args.source)
    (args.case / "analysis.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key != "waveform_windows"}, indent=2))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
