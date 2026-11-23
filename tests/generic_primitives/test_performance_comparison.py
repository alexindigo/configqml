#!/usr/bin/env python3
"""Parser/input controls for the matched performance comparison."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("comparison", Path(__file__).with_name("compare-performance.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


class ComparisonTest(unittest.TestCase):
    def test_empty_spans_are_not_success(self):
        with self.assertRaises(RuntimeError):
            comparison.spans("Totals: 10 passed", "LEGACY_PERFORMANCE_SPAN")

    def test_qt_prefixed_measurements(self):
        text = 'QINFO : case() LEGACY_PERFORMANCE_SPAN {"leaves":32,"samples":["1","2"]}\n'
        self.assertEqual(comparison.spans(text, "LEGACY_PERFORMANCE_SPAN"),
                         [{"leaves": 32, "samples": ["1", "2"]}])

    def test_malformed_measurement_is_not_ignored(self):
        with self.assertRaises(ValueError):
            comparison.spans('LEGACY_PERFORMANCE_SPAN {"leaves":', "LEGACY_PERFORMANCE_SPAN")

    def test_cache_preserves_full_flag_values(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "CMakeCache.txt").write_text(
                '// comment\nCMAKE_CXX_FLAGS:STRING=-O2 -DNAME=a=b\n'
                'CMAKE_BUILD_TYPE:STRING=Release\n# ignored\n')
            self.assertEqual(comparison.cache(root), {
                "CMAKE_CXX_FLAGS": "-O2 -DNAME=a=b", "CMAKE_BUILD_TYPE": "Release"})

    def test_small_save_gate_uses_median_of_paired_ratios(self):
        runs = [(1000, 1150)] * 11 + [(10000, 10000)] * 9
        result = comparison.paired_save_summary(runs)
        self.assertEqual(result["median_ratio"], 1.15)
        self.assertTrue(result["passed"])

    def test_small_save_gate_rejects_regressions_and_missing_samples(self):
        self.assertFalse(comparison.paired_save_summary([(1000, 1151)] * 20)["passed"])
        for pairs in ([(1000, 1100)] * 19, [(0, 1100)] * 20, [(1000, 0)] * 20):
            with self.subTest(pairs=pairs), self.assertRaises(RuntimeError):
                comparison.paired_save_summary(pairs)


if __name__ == "__main__":
    unittest.main()
