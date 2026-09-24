#!/usr/bin/env python3
"""Regression checks for LFO measurements, with known modulation clocks."""
import importlib.util
import math
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("practical_gate", Path(__file__).with_name("practical-gate.py"))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class TremoloPeriodTest(unittest.TestCase):
    def test_carrier_ripple_does_not_set_tremolo_period(self):
        rate = 49170
        for hz in (1.85, 3.7, 7.4):
            for depth in (1.125, 4.875):
                with self.subTest(hz=hz, depth=depth):
                    pcm = []
                    for i in range(rate * 2):
                        triangle = 1.0 - abs(2.0 * ((i * hz / rate) % 1.0) - 1.0)
                        gain = 10.0 ** (-depth * triangle / 20.0)
                        pcm.append(int(12000 * gain * math.sin(2 * math.pi * 437.72 * i / rate)))
                    result = gate.tremolo_metrics(pcm, rate, 0.1, 1.9)
                    self.assertAlmostEqual(result["period_s"], 1.0 / hz, delta=0.006)

    def test_no_period_for_constant_or_short_input(self):
        for values in ([], [1.0], [1.0] * 100):
            self.assertEqual(gate.period_windows(values), 0)


if __name__ == "__main__":
    unittest.main()
