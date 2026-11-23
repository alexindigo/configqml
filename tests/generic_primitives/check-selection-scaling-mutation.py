#!/usr/bin/env python3
"""Reject the old all-pairs selection scan at the actual prefix boundary."""
import argparse
from pathlib import Path

from harness import digest, guest_environment, require_marker, run, write_json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
source, build, output = args.source.resolve(), args.build.resolve(), args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
env = guest_environment()
env["QT_FORCE_STDERR_LOGGING"] = "1"
names = run(output, "symbols", ["nm", "-D", "--defined-only", "--format=posix",
    build / "libconfigqml_core.so.0.5"], env=env)
symbols = {}
for stem in ("putValue", "startsWith"):
    found = [line.split()[0] for line in names.splitlines() if stem in line.split()[0]]
    if len(found) != 1:
        raise RuntimeError("nonunique structural boundary: " + stem)
    symbols[stem] = found[0]
fixture = output / "fixture-build"
run(output, "configure", ["cmake", "-S", source / "tests/generic_primitives/performance",
    "-B", fixture, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCONFIGQML_MEASURE_HANDLES=ON",
    "-DCONFIGQML_PUT_VALUE_SYMBOL=" + symbols["putValue"],
    "-DCONFIGQML_STARTS_WITH_SYMBOL=" + symbols["startsWith"],
    "-DCONFIGQML_SOURCE=" + str(source), "-DCONFIGQML_BUILD=" + str(build)], env=env)
run(output, "fixture-build", ["cmake", "--build", fixture, "--parallel", "2",
    "--target", "layer-structure"], env=env)
path = source / "configcandidate.cpp"
original = path.read_bytes()
before = digest(path)
start = b"    QSet<ConfigPath> seen, included, allowedAncestors;"
end = b"    if (selection.leaves.isEmpty())"
if original.count(start) != 1 or original.count(end) != 1:
    raise RuntimeError("nonunique selection capture implementation")
old = b"""    QSet<ConfigPath> seen, included;
    if (!schema.valid() || (paths.isEmpty() && !emptyMeansAll))
        selection.error = failure;
    for (const auto &query : paths) {
        if (!schema.descriptor(query) || (rejectDuplicates && seen.contains(query))) {
            selection.error = failure;
            break;
        }
        seen.insert(ownPath(query));
        bool found = false;
        for (const auto &leaf : allowed)
            if (startsWith(leaf, query)) {
                included.insert(leaf);
                found = true;
            }
        if (!found)
            selection.error = failure;
    }
    for (const auto &leaf : schema.leaves())
        if (allowed.contains(leaf) &&
            ((paths.isEmpty() && emptyMeansAll) || included.contains(leaf)))
            selection.leaves.append(leaf);
"""
command = [fixture / "layer-structure", "selectionUsesAncestorIndex"]
try:
    path.write_bytes(original[:original.index(start)] + old + original[original.index(end):])
    run(output, "mutated-build", ["cmake", "--build", build, "--parallel", "2",
        "--target", "configqml_core"], env=env)
    text = run(output, "mutated", command, env=env, expected=4)
    require_marker(text, "2 passed, 4 failed, 0 skipped")
    require_marker(text, "prefix_scans= 262144")
finally:
    path.write_bytes(original)
    if digest(path) != before:
        raise RuntimeError("selection mutation did not restore production bytes")
    run(output, "restored-build", ["cmake", "--build", build, "--parallel", "2",
        "--target", "configqml_core"], env=env)
require_marker(run(output, "restored", command, env=env), "6 passed, 0 failed, 0 skipped")
require_marker(run(output, "validation-oracle", [build / "test_generic_primitives",
    "selectionAncestorIndexPreservesValidation"], env=env), "3 passed, 0 failed, 0 skipped")
write_json(output / "result.json", {"source_sha256": before, "restored_sha256": digest(path),
    "all_pairs_selection_rejected": True})
print("SELECTION_SCALING_MUTATION_OK")
