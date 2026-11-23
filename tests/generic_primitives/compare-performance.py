#!/usr/bin/env python3
"""Matched old-public-workflow distributions plus candidate-only P1/P2/P3 gates.

No baseline acknowledgment is invented. Ratios are reported, not compared to an
arbitrary new threshold. The success marker requires the existing caller/model
budgets and structural assertions, not merely successful benchmark startup.
"""
import argparse
import json
import os
from pathlib import Path

from harness import digest, guest_environment, require_marker, run, write_json
from performance_spans import distribution


def cache(build):
    result = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("//", "#")) and "=" in line and ":" in line.split("=", 1)[0]:
            key, value = line.split("=", 1)
            result[key.split(":", 1)[0]] = value
    return result


def spans(text, marker):
    rows = [json.loads(line.split(marker + " ", 1)[1]) for line in text.splitlines()
            if marker + " " in line]
    if not rows:
        raise RuntimeError("empty performance span discovery: " + marker)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("baseline-source", "baseline-build", "candidate-source", "candidate-build", "output"):
        parser.add_argument("--" + key, type=Path, required=True)
    args = parser.parse_args()
    env = guest_environment()
    env["QT_FORCE_STDERR_LOGGING"] = "1"
    if Path("/proc/sys/kernel/sched_schedstats").read_text().strip() != "1":
        raise RuntimeError("full performance qualification requires enabled guest schedstats")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sources = {side: getattr(args, side + "_source").resolve() for side in ("baseline", "candidate")}
    builds = {side: getattr(args, side + "_build").resolve() for side in sources}
    caches = {side: cache(build) for side, build in builds.items()}
    keys = ("CMAKE_CXX_COMPILER", "CMAKE_BUILD_TYPE", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE",
            "CMAKE_EXE_LINKER_FLAGS", "CMAKE_SHARED_LINKER_FLAGS")
    flags = {side: {key: data.get(key, "") for key in keys} for side, data in caches.items()}
    if flags["baseline"] != flags["candidate"] or flags["baseline"]["CMAKE_BUILD_TYPE"] != "Release":
        raise RuntimeError("baseline/candidate compiler, build type or flags differ")
    compiler = flags["candidate"]["CMAKE_CXX_COMPILER"]
    run(output, "compiler", [compiler, "--version"], env=env)
    require_marker(run(output, "qt-version", ["pkg-config", "--modversion", "Qt6Core"], env=env), "6.12.0")
    fixture = sources["candidate"] / "tests/generic_primitives/performance"
    metadata = {"flags": flags, "guest_uuid": os.environ["CONFIGQML_TEST_VM_UUID"],
                "fixture": {path.name: digest(path) for path in sorted(fixture.glob("*")) if path.is_file()},
                "sources": {}, "libraries": {}}
    executables = {}
    for side in sources:
        metadata["sources"][side] = {str(path.relative_to(sources[side])): digest(path)
            for path in sorted(sources[side].rglob("*"))
            if path.is_file() and "__pycache__" not in path.parts}
        library = builds[side] / "libconfigqml_core.so.0.5"
        metadata["libraries"][side] = {"path": str(library.resolve()), "sha256": digest(library)}
        names = run(output, side + "-symbols", ["nm", "-D", "--defined-only", "--format=posix", library], env=env)
        symbols = []
        prefix_symbols = []
        for line in names.splitlines():
            name = line.split()[0]
            if "startsWith" in name:
                text = run(output, side + "-prefix-name", ["c++filt", name], env=env)
                if text.startswith("configqml::startsWith("):
                    prefix_symbols.append(name)
            if "putValue" in name:
                text = run(output, side + "-put-value-name", ["c++filt", name], env=env)
                if text.startswith("configqml::putValue("):
                    symbols.append(name)
        if len(symbols) != 1 or len(prefix_symbols) != 1:
            raise RuntimeError("actual per-leaf putValue boundary is not unique")
        metadata["libraries"][side]["put_value_symbol"] = symbols[0]
        build = output / (side + "-fixture")
        run(output, side + "-configure", ["cmake", "-S", fixture, "-B", build, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=" + compiler,
            "-DCMAKE_CXX_FLAGS=" + flags[side]["CMAKE_CXX_FLAGS"],
            "-DCMAKE_CXX_FLAGS_RELEASE=" + flags[side]["CMAKE_CXX_FLAGS_RELEASE"],
            "-DCMAKE_EXE_LINKER_FLAGS=" + flags[side]["CMAKE_EXE_LINKER_FLAGS"],
            "-DCONFIGQML_MEASURE_HANDLES=" + ("ON" if side == "candidate" else "OFF"),
             "-DCONFIGQML_PUT_VALUE_SYMBOL=" + symbols[0],
             "-DCONFIGQML_STARTS_WITH_SYMBOL=" + prefix_symbols[0],
            "-DCONFIGQML_SOURCE=" + str(sources[side]),
            "-DCONFIGQML_BUILD=" + str(builds[side])], env=env)
        run(output, side + "-build", ["cmake", "--build", build, "--parallel", "2"], env=env)
        executables[side] = build / "legacy-performance"
        linkage = run(output, side + "-loader", ["ldd", executables[side]], env=env)
        if "not found" in linkage or str(builds[side]) + "/libconfigqml_core" not in linkage:
            raise RuntimeError("performance fixture escaped its matched library")
    write_json(output / "inputs.json", metadata)
    structural = run(output, "candidate-effective-structure", [output / "candidate-fixture/layer-structure"], env=env)
    require_marker(structural, "15 passed, 0 failed, 0 skipped")
    results = []
    span_names = ("reset_dispatch_ns", "reset_e2e_ns", "set_ingress_ns",
                  "set_completion_including_consumers_ns",
                  "isolated_set_completion_including_consumers_ns", "cached_getter_10000_ns")
    expected = {(n, 1, 1) for n in (1, 32, 128, 512)} | {
        (128, 4, 1), (128, 16, 1), (128, 4, 8), (128, 4, 32)}
    for repeat in range(4):
        sides = ["baseline", "candidate"] if repeat % 2 == 0 else ["candidate", "baseline"]
        for side in sides:
            name = f"{repeat + 1}-{side}"
            text = run(output, name, [executables[side]], env=env)
            require_marker(text, "10 passed, 0 failed, 0 skipped")
            rows = spans(text, "LEGACY_PERFORMANCE_SPAN")
            if len(rows) != 8 or {(row["leaves"], row["sources"], row["readers"]) for row in rows} != expected:
                raise RuntimeError("legacy dimensions missing or duplicated")
            for row in rows:
                if row["reader_value_notifications"] != [row["readers"]] * 9:
                    raise RuntimeError("reader fan-out contract changed")
                for span in span_names:
                    values = [int(value) for value in row[span]]
                    if len(values) != 9 or any(value < 0 for value in values):
                        raise RuntimeError("incomplete measured distribution")
                    row[span] = values
            results.append({"run": name, "side": side, "rows": rows,
                            "executable_sha256": digest(executables[side])})
            write_json(output / "runs.json", results)
    text = run(output, "candidate-structural", [builds["candidate"] / "test_generic_performance"], env=env)
    generic = spans(text, "GENERIC_PERFORMANCE_SPAN")
    if {row["leaves"] for row in generic} != {1, 32, 128, 512} or len(generic) != 4:
        raise RuntimeError("candidate batch scaling dimensions missing")
    text = run(output, "candidate-handles", [output / "candidate-fixture/handle-performance"], env=env)
    handles = spans(text, "GENERIC_HANDLE_PERFORMANCE_SPAN")
    if len(handles) != 4 or {row["leaves"] for row in handles} != {1, 32, 128, 512}:
        raise RuntimeError("candidate handle scaling dimensions missing")
    write_json(output / "candidate-spans.json", {"batches": generic, "handles": handles})
    budget = run(output, "candidate-budgets", ["ctest", "--test-dir", builds["candidate"],
        "-R", "^(thread_work|models|qml_models|json_files|compact_events|source_events)$",
        "--verbose", "--no-tests=error"], env=env)
    require_marker(budget, "100% tests passed out of 6")
    if "CALLER_WORK_BLOCKED_NOT_MEASURED" in budget:
        raise RuntimeError("release-qualification blocked-time gate was not measured")
    summaries = []
    lines = ["# Matched public-workflow performance", "",
        "Same compiler, flags, VM and fixture, four alternating pairs. No new ratio threshold.",
        "Reset/completion spans include worker delivery and fixture consumers, not only library CPU.",
        "Combined completion includes queued Discard capture; isolated completion follows its drained write.",
        "New handle/observation spans have no v0.5.3 equivalent; their structural assertions and",
        "existing caller/model budgets are separate candidate-only gates.", "",
        "| # | Leaves | Sources | Readers | Span | Baseline median/p95 ms | Candidate median/p95 ms | Median ratio |",
        "|---|---|---|---|---|---|---|---|"]
    for dimensions in sorted(expected):
        for span in span_names:
            distributions = {}
            for side in sources:
                values = [value for result in results if result["side"] == side for row in result["rows"]
                    if (row["leaves"], row["sources"], row["readers"]) == dimensions for value in row[span]]
                distributions[side] = distribution(values)
            baseline, candidate = distributions["baseline"], distributions["candidate"]
            ratio = candidate["median"] / baseline["median"] if baseline["median"] else None
            summaries.append({"dimensions": dimensions, "span": span, "distributions": distributions,
                              "median_ratio": ratio})
            def ms(data):
                return f'{data["median"] / 1e6:.3f}/{data["p95"] / 1e6:.3f}'
            lines.append(f'| {len(summaries)} | {dimensions[0]} | {dimensions[1]} | {dimensions[2]} | '
                         f'{span} | {ms(baseline)} | {ms(candidate)} | {ratio:.3f} |')
    write_json(output / "summary.json", {"comparisons": summaries,
        "candidate_existing_budgets_passed": True, "candidate_structural_scaling_passed": True,
        "boundary": "measured-workflows-not-a-domain-activation-promise"})
    (output / "report.md").write_text("\n".join(lines) + "\n")
    for side, source in sources.items():
        if metadata["sources"][side] != {str(path.relative_to(source)): digest(path)
                for path in sorted(source.rglob("*")) if path.is_file() and "__pycache__" not in path.parts}:
            raise RuntimeError("source inputs changed during comparison")
        if digest(builds[side] / "libconfigqml_core.so.0.5") != metadata["libraries"][side]["sha256"]:
            raise RuntimeError("library changed during comparison")
    print("GENERIC_PERFORMANCE_OK")


if __name__ == "__main__":
    main()
