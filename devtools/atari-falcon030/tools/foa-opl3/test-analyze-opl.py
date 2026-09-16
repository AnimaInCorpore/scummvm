#!/usr/bin/env python3
"""Unit tests for the OPL trace analysis, using synthetic traces."""
import importlib.util
from pathlib import Path
import unittest

_spec = importlib.util.spec_from_file_location("analyze_opl", Path(__file__).resolve().parent / "analyze-opl.py")
analyze_opl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(analyze_opl)

HEADER = "# test trace\n"
FOOTER = "# end\n"


def trace(*lines):
    return HEADER + "".join(line + "\n" for line in lines) + FOOTER


class ClassifyTest(unittest.TestCase):
    def test_operator_registers_map_to_channel_and_slot(self):
        # Channel 0 modulator/carrier, then channel 8's pair.
        self.assertEqual(analyze_opl.classify(0x20), ("am_vib_eg_ksr_mult", 0, 0))
        self.assertEqual(analyze_opl.classify(0x23), ("am_vib_eg_ksr_mult", 0, 1))
        self.assertEqual(analyze_opl.classify(0xF5), ("waveform_select", 8, 1))
        self.assertEqual(analyze_opl.classify(0x55), ("ksl_tl", 8, 1))

    def test_channel_and_control_registers(self):
        self.assertEqual(analyze_opl.classify(0xA3), ("fnum_low", 3, None))
        self.assertEqual(analyze_opl.classify(0xB8), ("keyon_block_fnum_high", 8, None))
        self.assertEqual(analyze_opl.classify(0xC0), ("feedback_connection", 0, None))
        self.assertEqual(analyze_opl.classify(0xBD), ("rhythm_depth", None, None))
        self.assertEqual(analyze_opl.classify(0x01), ("chip_control", None, None))

    def test_gaps_between_operator_blocks_are_not_claimed(self):
        # 0x06, 0x1E and 0x56 sit in the holes of the operator offset map.
        for reg in (0x26, 0x3E, 0x56):
            self.assertEqual(analyze_opl.classify(reg)[0], "other")

    def test_second_register_bank_is_labelled(self):
        self.assertEqual(analyze_opl.classify(0x1B0), ("secondary_keyon_block_fnum_high", 0, None))


class WindowPeakTest(unittest.TestCase):
    def test_window_is_half_open(self):
        self.assertEqual(analyze_opl.window_peak([0, 999, 1000], 1000), 2)

    def test_empty_and_single(self):
        self.assertEqual(analyze_opl.window_peak([], 1000), 0)
        self.assertEqual(analyze_opl.window_peak([7], 1000), 1)


class ParseTest(unittest.TestCase):
    def test_unclosed_trace_is_rejected(self):
        with self.assertRaises(ValueError):
            analyze_opl.parse(HEADER + "T 0\n")

    def test_unknown_record_is_rejected(self):
        with self.assertRaises(ValueError):
            analyze_opl.parse(trace("X 0"))


class AnalyzeTest(unittest.TestCase):
    def test_key_edges_retriggers_and_peak_occupancy(self):
        result = analyze_opl.analyze(trace(
            "I 0 init", "I 0 start 250 Hz",
            "T 0", "W 0 t 0b0 30", "W 0 t 0b1 30",
            "T 4000", "W 4000 t 0b0 30",            # retrigger, still keyed
            "T 8000", "W 8000 t 0b0 10", "W 8000 t 0b1 10",
            "T 12000", "I 12000 stop"))
        keyed = result["keyed_channels"]
        self.assertEqual((keyed["key_on_edges"], keyed["key_off_edges"]), (2, 2))
        self.assertEqual(keyed["retriggers_without_release"], 1)
        self.assertEqual(keyed["peak"], 2)
        self.assertEqual(result["callback_ticks"], 4)
        self.assertEqual(result["callback_period_us"], 4000)
        self.assertEqual(result["callback_periods_off_grid"], 0)

    def test_writes_after_stop_are_reported_separately(self):
        result = analyze_opl.analyze(trace(
            "T 0", "W 0 t 0a0 01", "I 4000 stop", "W 4000 g 0b0 10", "I 4000 close"))
        self.assertEqual(result["writes"], 1)
        self.assertEqual(result["shutdown_writes"], 1)
        self.assertEqual(result["markers"], ["stop", "close"])

    def test_ignored_bits_and_effective_waveform(self):
        result = analyze_opl.analyze(trace("T 0", "W 0 t 0e0 4e", "W 0 t 0e3 02", "I 0 stop"))
        self.assertEqual(result["writes_with_bits_the_opl2_ignores"], {"waveform_select": 1})
        self.assertEqual(result["waveform_select_effective"], {"2": 2})
        self.assertEqual(result["waveform_select_raw_values"], {"0x02": 1, "0x4e": 1})

    def test_context_split_and_off_grid_callbacks(self):
        result = analyze_opl.analyze(trace(
            "T 0", "W 0 t 0a0 01", "W 1500 g 0a1 02", "T 4000", "T 9000", "I 9000 stop"))
        self.assertEqual(result["write_context"], {"driver_callback": 1, "engine_thread": 1})
        self.assertEqual(result["callback_periods_off_grid"], 1)
        self.assertEqual(result["writes_in_busiest_callback"], 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
