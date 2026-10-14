#!/usr/bin/env python3
"""Check benchmark aggregation using deliberately uneven file sizes/rates."""
import contextlib
import fcntl
import io
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import bench_boltlz as benchmark
from bench_boltlz import CODECS, markdown_report, summarize
from experiment_boltlz import sampled_corpus


class BenchmarkAggregation(unittest.TestCase):
    def test_tuning_samples_preserve_original_block_boundaries(self):
        window = 262144
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "input"
            source.mkdir()
            original = b"".join(bytes([i]) * window for i in range(6)) + b"final seventeen!!"
            (source / "large").write_bytes(original)
            (source / "small").write_bytes(b"small input")
            (source / "linked").symlink_to(source / "large")
            destination = root / "sample"
            manifest = sampled_corpus(source, destination)
            self.assertEqual(set(manifest), {"large", "small"})
            self.assertEqual((destination / "large").read_bytes(),
                             bytes(window) + bytes([3]) * window + b"final seventeen!!")
            self.assertEqual((destination / "small").read_bytes(), b"small input")
            with self.assertRaises(ValueError):
                sampled_corpus(source, source)
            self.assertEqual((source / "large").read_bytes(), original)
            self.assertTrue(destination.with_suffix(".manifest.json").exists())
            self.assertFalse((destination / "linked").exists())
            for start, length in manifest["large"]["spans"]:
                self.assertEqual(start % window, 0)
                self.assertLessEqual(length, window)

    def test_weighted_rates_controls_and_block_filtering(self):
        cases = [
            {"name": "large", "size": 900, "category": "real", "control": False},
            {"name": "small", "size": 100, "category": "real", "control": False},
            {"name": "zeros", "size": 1000000, "category": "control", "control": True},
        ]
        samples = []
        for codec in CODECS:
            for case, rate in zip(cases, (100, 10, 10000)):
                for multiplier in (0.5, 1, 2):
                    samples.append({"codec": codec, "file": case["name"], "block_size": 32768,
                                    "input_bytes": case["size"], "encoded_bytes": case["size"] // 2,
                                    "effective_bytes": case["size"] // 2,
                                    "encode_bytes": rate * multiplier, "encode_seconds": 1,
                                    "decode_bytes": 2 * rate * multiplier, "decode_seconds": 1})
                samples.append({**samples[-1], "block_size": 65536, "encoded_bytes": 1})
        result = summarize(samples, cases, 32768)
        for codec in CODECS:
            totals = result[codec]["aggregate"]
            self.assertEqual(totals["input_bytes"], 1000)
            self.assertEqual(totals["encoded_bytes"], 500)
            self.assertAlmostEqual(totals["encode_mib_s"] * 1048576, 1000 / 19)
            self.assertAlmostEqual(totals["decode_mib_s"] * 1048576, 2000 / 19)
        text = markdown_report({"environment": {"cpu": "test", "compiler": "test", "flags": "test"},
                                "summary": {"32768": result}})
        self.assertIn("50.00%", text)

    def test_inconsistent_compressed_sizes_are_rejected(self):
        case = {"name": "input", "size": 100, "category": "real", "control": False}
        samples = [{"codec": "lz4", "file": "input", "block_size": 32768,
                    "encoded_bytes": size} for size in (50, 51)]
        with self.assertRaises(AssertionError):
            summarize(samples, [case], 32768, ["lz4"])

    def test_no_build_checks_binary_stamp_even_after_other_builds(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            flags = benchmark.DEFAULT_FLAGS
            (build / ".config").write_text(f"CC=gcc\nBOLTLZ_CFLAGS={flags}\n")
            (build / "bench_boltlz.config").write_text(f"CC=cc\nBOLTLZ_CFLAGS={flags}\n")
            error = io.StringIO()
            with patch.object(benchmark, "BUILD", build), \
                    patch("sys.argv", ["bench", "--no-build", "--cc", "gcc"]), \
                    contextlib.redirect_stderr(error):
                with self.assertRaises(SystemExit) as result:
                    benchmark.main()
            self.assertEqual(result.exception.code, 2)
            self.assertIn("do not match the build stamp", error.getvalue())

    def test_lz4_reference_and_language_flags(self):
        import hashlib
        import json
        reference = benchmark.ROOT / "test/reference/lz4"
        manifest = json.loads((reference / "upstream.json").read_text())
        self.assertEqual(manifest["version"], "1.10.0")
        for name, expected in manifest["sha256"].items():
            self.assertEqual(hashlib.sha256((reference / name).read_bytes()).hexdigest(), expected)
        flags = benchmark.lz4_flags(benchmark.DEFAULT_FLAGS)
        self.assertNotIn("-ansi", flags)
        self.assertIn("-std=c99", flags)
        self.assertIn("-O2", flags)
        self.assertIn("lz4", CODECS)
        self.assertIn("lz4-hc-9", CODECS)

    def test_frozen_speed_reference_hashes(self):
        import hashlib
        import json
        reference = benchmark.ROOT / "test/reference"
        for snapshot in ("speed", "review", "loop"):
            manifest = json.loads((reference / f"boltlz_{snapshot}_base.json").read_text())
            for name, expected in manifest["sha256"].items():
                self.assertEqual(hashlib.sha256((reference / name).read_bytes()).hexdigest(), expected)

    def test_paired_decoder_aggregation_and_codec_subset(self):
        cases = [{"name": "big", "size": 900, "control": False, "category": "test"},
                 {"name": "small", "size": 100, "control": False, "category": "test"}]
        samples = []
        for case, speed in zip(cases, (100, 10)):
            for multiplier in (0.5, 1, 2):
                samples.append({"codec": "boltlz-fast", "file": case["name"], "block_size": 32768,
                                "encoded_bytes": case["size"] // 2, "effective_bytes": case["size"] // 2,
                                "encode_bytes": speed, "encode_seconds": 1,
                                "decode_bytes": speed * 3 * multiplier, "decode_seconds": 1,
                                "frozen_decode_bytes": speed * multiplier, "frozen_decode_seconds": 1,
                                "encoder_identical": True})
        result = summarize(samples, cases, 32768, ["boltlz-fast"])
        self.assertEqual(set(result), {"boltlz-fast"})
        self.assertAlmostEqual(result["boltlz-fast"]["aggregate"]["decoder_speedup"], 3)
        self.assertTrue(result["boltlz-fast"]["aggregate"]["encoder_identical"])
        samples[0]["encoder_identical"] = False
        result = summarize(samples, cases, 32768, ["boltlz-fast"])
        self.assertFalse(result["boltlz-fast"]["aggregate"]["encoder_identical"])
        samples[0]["frozen_decoder"] = "review"
        with self.assertRaisesRegex(AssertionError, "mixed decoder baselines"):
            summarize(samples, cases, 32768, ["boltlz-fast"])

    def test_overlapping_benchmark_is_rejected_before_build(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            with (build / ".benchmark.lock").open("a+") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                error = io.StringIO()
                with patch.object(benchmark, "BUILD", build), \
                        patch("sys.argv", ["bench"]), contextlib.redirect_stderr(error):
                    with self.assertRaises(SystemExit) as result:
                        benchmark.main()
                self.assertEqual(result.exception.code, 2)
                self.assertIn("another BoltLZ benchmark is running", error.getvalue())


if __name__ == "__main__":
    unittest.main()
