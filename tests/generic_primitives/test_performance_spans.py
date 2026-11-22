#!/usr/bin/env python3
import copy
import unittest

from performance_spans import distribution, markdown, summarize


def sample(wall=20000000, cpu=4000000, queue=13000000):
    blocked = max(0, wall - cpu - queue)
    return {"label": "matched-core-ingress", "wall_ns": wall, "cpu_ns": cpu,
            "runqueue_ns": queue, "blocked_ns": blocked, "charged_ns": cpu + blocked,
            "voluntary": 1, "involuntary": 2, "valid": 1}


def runs():
    return [{"name": "1-release-baseline", "rc": 0, "seconds": 4.25,
             "samples": [sample(), sample(50000000, 40000000, 1000000)]},
            {"name": "1-release-candidate", "rc": 1, "seconds": 5.5,
             "samples": [sample(40000000, 100000, 0)]}]


class PerformanceSpansTest(unittest.TestCase):
    def test_observed_quantiles_and_empty_refusal(self):
        self.assertEqual(distribution([9, 1, 3, 7]),
                         dict(count=4, min=1, median=5, p95=9, max=9, sum=20))
        with self.assertRaises(ValueError):
            distribution([])

    def test_preserves_failure_and_blocking(self):
        summary = summarize(runs())
        self.assertEqual(summary["boundary"], "measured-caller-spans-not-full-qualification")
        self.assertEqual(summary["runs"][1]["rc"], 1)
        candidate = next(g for g in summary["groups"] if g["side"] == "candidate")
        self.assertEqual(candidate["measurements"]["blocked_ns"]["max"], 39900000)
        self.assertEqual(candidate["measurements"]["charged_ns"]["max"], 40000000)
        self.assertIn("not full performance qualification", markdown(summary))
        self.assertIn("| 1 | release | baseline |", markdown(summary))

    def test_never_hides_malformed_or_cpu_only_samples(self):
        for field, value in [("valid", 0), ("blocked_ns", 0), ("charged_ns", 4000000),
                             ("runqueue_ns", -1)]:
            broken = runs()
            broken[0]["samples"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                summarize(broken)
        with self.assertRaises(ValueError):
            summarize([])
        with self.assertRaises(ValueError):
            summarize(runs() + [copy.deepcopy(runs()[0])])
        broken = runs()
        broken[0]["rc"] = 124
        with self.assertRaises(ValueError):
            summarize(broken)


if __name__ == "__main__":
    unittest.main()
