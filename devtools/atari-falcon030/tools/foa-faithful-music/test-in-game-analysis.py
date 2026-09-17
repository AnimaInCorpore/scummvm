#!/usr/bin/env python3
"""Synthetic positive/negative controls for the in-game audio acceptance gate."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import wave

import numpy as np

spec = importlib.util.spec_from_file_location("analysis", Path(__file__).with_name("analyze-in-game.py"))
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class AnalysisTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.case = Path(self.directory.name)
        app = self.case / "HD/SCUMMVM"
        app.mkdir(parents=True)
        self.rate = round(analysis.RATE)
        self.source = np.random.default_rng(1).integers(-5000, 5000, (4 * self.rate, 2), dtype=np.int16)
        self.source.astype(">i2").tofile(app / "MUSIC.RAW")
        self.stats = dict(source_sha256="synthetic", binary_sha256="synthetic", start_ms=1000,
                          end_ms=5000, samples_per_half=9834, consumed_bytes=self.source.nbytes,
                          mix_ms=400, min_queued_bytes=131072, dma_stops=0, missing_bytes=0,
                          speech_mixes=20, speech_requested=True, clock_resolution_ms=5,
                          probe_report_complete=1, irq_log_overflow=0, mix_log_overflow=0)
        self.write_stats()
        irqs = [f"{1200 + i * 200},0,10" for i in range(20)]
        mixes = [f"{1002 + i * 200},{1022 + i * 200},{(i + 1) * 9834 * 4},131072,1,9834" for i in range(20)]
        (app / "PCMSTAT.TXT").write_text("irq_ms,stopped,update_age_ms\n" + "\n".join(irqs) +
                                       "\nmix_begin_ms,mix_end_ms,consumed_bytes,queued_bytes,speech,frames\n" +
                                       "\n".join(mixes) + "\nprobe_report_complete=1\n")

    def write_stats(self):
        (self.case / "result.json").write_text(json.dumps(self.stats))

    def capture(self, samples):
        # Centered mono content and common gain must leave L-R alignment valid.
        mixed = samples.astype(np.int32) // 2
        mono = (1200 * np.sin(np.arange(len(samples)) * .03)).astype(np.int16)
        mixed += mono[:, None]
        capture = np.concatenate([np.zeros((1000, 2), dtype=np.int16), mixed.astype(np.int16)])
        with wave.open(str(self.case / "output.wav"), "wb") as wav:
            wav.setparams((2, 2, self.rate, 0, "NONE", "not compressed"))
            wav.writeframes(capture.astype("<i2").tobytes())

    def test_continuous_music_with_mono_speech(self):
        self.capture(self.source)
        result = analysis.analyze(self.case)
        self.assertEqual(result["channel_order"], "normal")
        self.assertTrue(result["passed"])

    def test_live_source_must_be_explicit(self):
        self.capture(self.source)
        self.stats["live_synthesis"] = 1
        self.write_stats()
        with self.assertRaisesRegex(ValueError, "independently generated"):
            analysis.analyze(self.case)
        source = self.case / "independent.raw"
        self.source.astype(">i2").tofile(source)
        # In a live run MUSIC.RAW is the FCM package, not recorded music.
        (self.case / "HD/SCUMMVM/MUSIC.RAW").write_bytes(b"FCM1")
        self.assertTrue(analysis.analyze(self.case, source)["passed"])

    def test_swapped_channels_are_reported_and_rejected(self):
        self.capture(self.source[:, ::-1])
        result = analysis.analyze(self.case)
        self.assertEqual(result["channel_order"], "swapped")
        self.assertEqual(result["low_correlation_windows"], 0)
        self.assertFalse(result["passed"])

    def test_silent_gap_without_dma_stop_is_rejected(self):
        samples = self.source.copy()
        samples[2 * self.rate:2 * self.rate + self.rate // 50] = 0
        self.capture(samples)
        self.assertFalse(analysis.analyze(self.case)["passed"])

    def test_repeated_block_without_dma_stop_is_rejected(self):
        samples = self.source.copy()
        block = self.rate // 5
        samples[2 * self.rate:2 * self.rate + block] = samples[2 * self.rate - block:2 * self.rate]
        self.capture(samples)
        self.assertFalse(analysis.analyze(self.case)["passed"])

    def test_sub_tick_deadline_margin_is_rejected(self):
        self.capture(self.source)
        path = self.case / "HD/SCUMMVM/PCMSTAT.TXT"
        path.write_text(path.read_text().replace("1002,1022,", "1002,1195,"))
        self.assertFalse(analysis.analyze(self.case)["passed"])

    def test_active_dma_destination_is_rejected_even_with_continuous_audio(self):
        self.capture(self.source)
        for field in ("dma_active_before_write", "dma_active_after_write"):
            with self.subTest(field=field):
                self.stats.update(dma_active_before_write=0, dma_active_after_write=0)
                self.stats[field] = 1
                self.write_stats()
                result = analysis.analyze(self.case)
                self.assertEqual(result["low_correlation_windows"], 0)
                self.assertFalse(result["passed"])

    def test_inactive_dma_destination_with_continuous_audio_passes(self):
        self.capture(self.source)
        self.stats.update(dma_active_before_write=0, dma_active_after_write=0)
        self.write_stats()
        self.assertTrue(analysis.analyze(self.case)["passed"])


if __name__ == "__main__":
    unittest.main()
