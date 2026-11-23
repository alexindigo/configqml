#!/usr/bin/env python3
"""Reject repeated empty-candidate resolution and unused projection assembly."""
import argparse
from pathlib import Path

from harness import digest, guest_environment, require_marker, run, write_json

parser = argparse.ArgumentParser(description=__doc__)
for key in ("source", "build", "output"):
    parser.add_argument("--" + key, type=Path, required=True)
args = parser.parse_args()
source, build, output = args.source.resolve(), args.build.resolve(), args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
env = dict(guest_environment(), QT_FORCE_STDERR_LOGGING="1")
names = run(output, "symbols", ["nm", "-D", "--defined-only", "--format=posix",
    build / "libconfigqml_core.so.0.5"], env=env)
symbols = {}
for key, fragment in (("PUT_VALUE", "putValue"), ("STARTS_WITH", "startsWith"),
                      ("INFO", "ConfigLayers4info")):
    found = [line.split()[0] for line in names.splitlines() if fragment in line.split()[0]]
    if len(found) != 1:
        raise RuntimeError("nonunique actual boundary: " + fragment)
    symbols[key] = found[0]
fixture = output / "fixture"
run(output, "configure", ["cmake", "-S", source / "tests/generic_primitives/performance",
    "-B", fixture, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCONFIGQML_MEASURE_HANDLES=ON",
    "-DCONFIGQML_SOURCE=" + str(source), "-DCONFIGQML_BUILD=" + str(build),
    *["-DCONFIGQML_" + key + "_SYMBOL=" + value for key, value in symbols.items()]], env=env)
run(output, "fixture-build", ["cmake", "--build", fixture, "--parallel", "2",
    "--target", "layer-structure"], env=env)
controls = [
    ("initial-empty", "configdraftpreview.cpp",
     b"batch.error.isEmpty() && batch.operations.isEmpty() && !previous.frozen",
     b"false", [build / "test_generic_performance", "emptyDraftUsesPublicationCut"],
     "Actual   (before)"),
    ("ordinary-empty", "configdraftpreview.cpp",
     b"if (it->batch.operations.isEmpty()) {\n            // This publication",
     b"if (false) {\n            // This publication",
     [build / "test_generic_performance", "emptyDraftUsesPublicationCut"],
     "extra_resolutions= 4"),
    ("unused-lower", "configlayers.cpp",
     b"m_projections.isEmpty() ? QMap<ConfigPath, QVariant>{}",
     b"false ? QMap<ConfigPath, QVariant>{}",
     [fixture / "layer-structure", "noProjectionDoesNotAssembleLower"],
     "value_queries= 512"),
]
receipts = []
for label, filename, boundary, mutation, command, marker in controls:
    path = source / filename
    original = path.read_bytes()
    before = digest(path)
    if original.count(boundary) != 1:
        raise RuntimeError("nonunique mutation boundary: " + label)
    try:
        path.write_bytes(original.replace(boundary, mutation))
        run(output, label + "-build", ["cmake", "--build", build, "--parallel", "2",
            "--target", "configqml_core"], env=env)
        text = run(output, label, command, env=env, expected=4)
        require_marker(text, "2 passed, 4 failed, 0 skipped")
        require_marker(text, marker)
    finally:
        path.write_bytes(original)
        if digest(path) != before:
            raise RuntimeError("mutation did not restore source: " + label)
        run(output, label + "-restored-build", ["cmake", "--build", build,
            "--parallel", "2", "--target", "configqml_core"], env=env)
    receipts.append(dict(label=label, source_sha256=before, restored_sha256=digest(path)))
require_marker(run(output, "restored-native", [build / "test_generic_performance"], env=env),
    "12 passed, 0 failed, 0 skipped")
require_marker(run(output, "restored-structure", [fixture / "layer-structure"], env=env),
    "27 passed, 0 failed, 0 skipped")
write_json(output / "result.json", {"controls": receipts, "all_rejected": True})
print("ADMITTED_CANDIDATE_MUTATION_OK")
