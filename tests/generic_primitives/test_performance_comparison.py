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


if __name__ == "__main__":
    unittest.main()
