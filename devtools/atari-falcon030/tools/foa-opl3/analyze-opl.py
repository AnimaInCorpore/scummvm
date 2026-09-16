#!/usr/bin/env python3
"""Summarize a captured OPL register stream for DSP renderer sizing.

Everything here describes one recorded run. The counts are measurements of
that run's register traffic and keyed-voice occupancy. They are not a cost
model for any renderer, and nothing here is an audio comparison.
"""
import argparse
import bisect
from collections import Counter
import json
from pathlib import Path

# OPL operator register offsets in channel order: each channel's modulator
# offset, with its carrier three higher.
OPERATOR1 = (0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12)
OPERATOR_SLOT = {}
for _channel, _base in enumerate(OPERATOR1):
    OPERATOR_SLOT[_base] = (_channel, 0)
    OPERATOR_SLOT[_base + 3] = (_channel, 1)

OPERATOR_BLOCKS = ((0x20, "am_vib_eg_ksr_mult"), (0x40, "ksl_tl"), (0x60, "attack_decay"),
                   (0x80, "sustain_release"), (0xE0, "waveform_select"))

# Bits a YM3812 actually decodes. The AdLib driver forwards raw instrument
# bytes, so a backend must mask rather than reject what it receives.
OPL2_SIGNIFICANT = {"waveform_select": 0x03, "feedback_connection": 0x0F,
                    "keyon_block_fnum_high": 0x3F}


def classify(reg):
    """Return (class name, channel or None, operator slot or None)."""
    bank, low = reg >> 8, reg & 0xFF
    prefix = "secondary_" if bank else ""
    if low == 0xBD:
        return prefix + "rhythm_depth", None, None
    if 0xA0 <= low <= 0xA8:
        return prefix + "fnum_low", low - 0xA0, None
    if 0xB0 <= low <= 0xB8:
        return prefix + "keyon_block_fnum_high", low - 0xB0, None
    if 0xC0 <= low <= 0xC8:
        return prefix + "feedback_connection", low - 0xC0, None
    for base, name in OPERATOR_BLOCKS:
        if low >= base:
            slot = OPERATOR_SLOT.get(low - base)
            if slot is not None:
                return prefix + name, slot[0], slot[1]
    if low in (0x01, 0x02, 0x03, 0x04, 0x05, 0x08):
        return prefix + "chip_control", None, None
    return prefix + "other", None, None


def window_peak(times, span_us):
    """Largest number of writes inside any half-open window of span_us."""
    peak = 0
    for index, start in enumerate(times):
        peak = max(peak, bisect.bisect_left(times, start + span_us) - index)
    return peak


def parse(text):
    """Split a trace into header comments and typed records."""
    if not text.endswith("# end\n"):
        raise ValueError("Trace is not closed")
    header, records = [], []
    for line in text.splitlines():
        if not line:
            continue
        if line.startswith("#"):
            header.append(line)
            continue
        fields = line.split()
        kind = fields[0]
        if kind == "T":
            records.append(("T", int(fields[1]), None, None, None))
        elif kind == "W":
            records.append(("W", int(fields[1]), fields[2], int(fields[3], 16), int(fields[4], 16)))
        elif kind == "I":
            records.append(("I", int(fields[1]), None, None, " ".join(fields[2:])))
        else:
            raise ValueError(f"Unknown record: {line}")
    return header, records


def analyze(text):
    header, records = parse(text)
    # Writes after the driver stops are the harness closing the game down.
    limit = next((i for i, r in enumerate(records) if r[0] == "I" and r[4] == "stop"), len(records))
    live, shutdown = records[:limit], records[limit:]

    ticks = [r[1] for r in live if r[0] == "T"]
    classes, unused_bits, values, contexts = Counter(), Counter(), {}, Counter()
    per_tick, tick_index = Counter(), -1
    keyed, keyed_series, key_edges, peak_keyed = set(), [], Counter(), 0
    waveform_raw, waveform_used = Counter(), Counter()
    feedback, connection, modulation = Counter(), Counter(), Counter()
    times, secondary = [], 0

    for record in live:
        if record[0] == "T":
            tick_index += 1
            keyed_series.append(len(keyed))
            continue
        if record[0] != "W":
            continue
        _, when, context, reg, value = record
        times.append(when)
        name, channel, _slot = classify(reg)
        classes[name] += 1
        contexts[context] += 1
        values.setdefault(reg, Counter())[value] += 1
        secondary += reg >> 8 != 0
        if context == "t":
            per_tick[tick_index] += 1
        significant = OPL2_SIGNIFICANT.get(name)
        if significant is not None and value & ~significant:
            unused_bits[name] += 1
        if name == "keyon_block_fnum_high":
            on, was = bool(value & 0x20), channel in keyed
            if on and not was:
                keyed.add(channel)
                key_edges["on"] += 1
                peak_keyed = max(peak_keyed, len(keyed))
            elif on and was:
                # A retrigger with no intervening release: the driver reused
                # this hardware voice for another note.
                key_edges["retrigger"] += 1
            elif was:
                keyed.discard(channel)
                key_edges["off"] += 1
        elif name == "waveform_select":
            waveform_raw[value] += 1
            waveform_used[value & 0x03] += 1
        elif name == "feedback_connection":
            feedback[(value >> 1) & 0x07] += 1
            connection[value & 0x01] += 1
        elif name == "am_vib_eg_ksr_mult":
            modulation["tremolo"] += bool(value & 0x80)
            modulation["vibrato"] += bool(value & 0x40)
            modulation["sustaining"] += bool(value & 0x20)
            modulation["key_scale_rate"] += bool(value & 0x10)

    span = times[-1] - times[0] if len(times) > 1 else 0
    period = ticks[1] - ticks[0] if len(ticks) > 1 else 0
    off_grid = sum(1 for a, b in zip(ticks, ticks[1:]) if b - a != period)
    gaps = [b - a for a, b in zip(times, times[1:])]

    return {
        "scope": "One recorded run of the real game, iMUSE and AdLib driver; no synthesis, no audio",
        "header": header,
        "writes": len(times),
        "callback_ticks": len(ticks),
        "callback_period_us": period,
        "callback_periods_off_grid": off_grid,
        "trace_span_us": span,
        "writes_per_second": round(len(times) * 1e6 / span, 2) if span else 0.0,
        "write_context": {"driver_callback": contexts["t"], "engine_thread": contexts["g"]},
        "secondary_bank_writes": secondary,
        "register_classes": dict(classes.most_common()),
        "writes_with_bits_the_opl2_ignores": dict(unused_bits),
        "distinct_registers": len(values),
        "distinct_register_values": sum(len(v) for v in values.values()),
        "peak_writes_in_window": {f"{span_us // 1000}ms": window_peak(times, span_us)
                                  for span_us in (1000, 4000, 10000, 20000, 100000, 1000000)},
        "writes_in_busiest_callback": max(per_tick.values(), default=0),
        "callbacks_with_writes": len(per_tick),
        "longest_gap_between_writes_us": max(gaps, default=0),
        "keyed_channels": {
            "peak": peak_keyed,
            "peak_sampled_at_callbacks": max(keyed_series, default=0),
            "mean_at_callbacks": round(sum(keyed_series) / len(keyed_series), 3) if keyed_series else 0.0,
            "callback_histogram": {str(count): total for count, total in sorted(Counter(keyed_series).items())},
            "key_on_edges": key_edges["on"],
            "key_off_edges": key_edges["off"],
            "retriggers_without_release": key_edges["retrigger"],
        },
        "waveform_select_raw_values": {f"{v:#04x}": n for v, n in sorted(waveform_raw.items())},
        "waveform_select_effective": {str(v): n for v, n in sorted(waveform_used.items())},
        "feedback_values": {str(v): n for v, n in sorted(feedback.items())},
        "connection_values": {str(v): n for v, n in sorted(connection.items())},
        "operator_flag_writes": dict(modulation),
        "rhythm_register_writes": sum(values.get(0xBD, Counter()).values()),
        "markers": [r[4] for r in records if r[0] == "I"],
        "shutdown_writes": sum(1 for r in shutdown if r[0] == "W"),
        "audio_compared": False,
        "dsp_cost_measured": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.trace.read_text())
    if args.output:
        with args.output.open("x") as handle:
            json.dump(result, handle, indent=2)
            handle.write("\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
