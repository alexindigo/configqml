#!/usr/bin/env python3
"""Summarize measured spans without turning timing samples into qualification."""
import argparse
import json
from pathlib import Path
import statistics


FIELDS = ("wall_ns", "cpu_ns", "runqueue_ns", "blocked_ns", "charged_ns",
          "voluntary", "involuntary")


def distribution(values):
    ordered = sorted(values)
    if not ordered:
        raise ValueError("empty span distribution")
    # Nearest-rank p95 retains the actual observation, including small samples.
    rank = (95 * len(ordered) + 99) // 100
    return {"count": len(ordered), "min": ordered[0],
            "median": statistics.median(ordered), "p95": ordered[rank - 1],
            "max": ordered[-1], "sum": sum(ordered)}


def summarize(runs):
    if not isinstance(runs, list) or not runs:
        raise ValueError("no matched runs")
    seen = set()
    groups = {}
    results = []
    for run in runs:
        name = run["name"]
        parts = name.split("-")
        if len(parts) != 3 or parts[1] not in ("release", "san") or parts[2] not in (
                "baseline", "candidate"):
            raise ValueError("unrecognized matched run identity: " + name)
        if name in seen:
            raise ValueError("duplicate matched run: " + name)
        seen.add(name)
        samples = run["samples"]
        if not samples:
            raise ValueError("missing measured spans: " + name)
        if run["rc"] not in (0, 1):
            raise ValueError("crash, setup error or timeout is not a timing result: " + name)
        by_span = {}
        for sample in samples:
            if sample["valid"] != 1 or any(
                    not isinstance(sample[field], int) or sample[field] < 0 for field in FIELDS):
                raise ValueError("invalid thread accounting: " + name)
            if sample["blocked_ns"] != max(
                    0, sample["wall_ns"] - sample["cpu_ns"] - sample["runqueue_ns"]):
                raise ValueError("blocked-time accounting changed: " + name)
            if sample["charged_ns"] != sample["cpu_ns"] + sample["blocked_ns"]:
                raise ValueError("blocking was not charged: " + name)
            label = sample["label"]
            by_span.setdefault(label, []).append(sample)
            groups.setdefault((parts[1], parts[2], label), []).append(sample)
        results.append({"name": name, "rc": run["rc"], "seconds": run["seconds"],
                        "spans": {label: {field: distribution([row[field] for row in rows])
                                          for field in FIELDS}
                                  for label, rows in sorted(by_span.items())}})
    summaries = [dict(build=key[0], side=key[1], span=key[2],
                      measurements={field: distribution([row[field] for row in rows])
                                    for field in FIELDS})
                 for key, rows in sorted(groups.items())]
    return {"boundary": "measured-caller-spans-not-full-qualification",
            "runs": results, "groups": summaries}


def markdown(summary):
    lines = ["# Matched caller-work distributions", "",
             "These are measured spans, not full performance qualification. Delivery",
             "includes fixture consumers where its span label says so. Whole-case",
             "seconds include setup/teardown and are not the post-ingress deadline.", "",
             "| # | Build | Side | Span | Samples | CPU median/p95 ms | Blocked median/p95 ms | Run-queue max ms | Charged max ms |",
             "|---|---|---|---|---|---|---|---|---|"]
    for index, group in enumerate(summary["groups"], 1):
        m = group["measurements"]
        def ms(field, point):
            return f'{m[field][point] / 1000000:.3f}'
        lines.append(f'| {index} | {group["build"]} | {group["side"]} | {group["span"]} | '
                     f'{m["cpu_ns"]["count"]} | {ms("cpu_ns", "median")}/{ms("cpu_ns", "p95")} | '
                     f'{ms("blocked_ns", "median")}/{ms("blocked_ns", "p95")} | '
                     f'{ms("runqueue_ns", "max")} | {ms("charged_ns", "max")} |')
    lines += ["", "| # | Run | Exit | Whole-case seconds |", "|---|---|---|---|"]
    for index, run in enumerate(summary["runs"], 1):
        lines.append(f'| {index} | {run["name"]} | {run["rc"]} | {run["seconds"]:.3f} |')
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--markdown", type=Path)
    args = parser.parse_args()
    summary = summarize(json.loads(args.input.read_text()))
    args.output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    if args.markdown:
        args.markdown.write_text(markdown(summary))
    print("CALLER_SPAN_SUMMARY_OK")


if __name__ == "__main__":
    main()
