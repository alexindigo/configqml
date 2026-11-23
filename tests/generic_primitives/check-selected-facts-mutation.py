#!/usr/bin/env python3
"""Reject full provenance materialization in selected candidate capture."""
import argparse
from pathlib import Path

from harness import digest, guest_environment, require_marker, run, write_json

parser = argparse.ArgumentParser(description=__doc__)
for key in ("source", "build", "output"):
    parser.add_argument("--" + key, type=Path, required=True)
args = parser.parse_args()
source, build, output = args.source.resolve(), args.build.resolve(), args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
env = guest_environment()
env["QT_FORCE_STDERR_LOGGING"] = "1"
names = run(output, "symbols", ["nm", "-D", "--defined-only", "--format=posix",
    build / "libconfigqml_core.so.0.5"], env=env)
symbols = {}
for line in names.splitlines():
    name = line.split()[0]
    for key, fragment in (("PUT_VALUE", "putValue"), ("STARTS_WITH", "startsWith"),
                          ("INFO", "ConfigLayers4info")):
        if fragment in name:
            if key in symbols:
                raise RuntimeError("nonunique boundary: " + key)
            symbols[key] = name
if len(symbols) != 3:
    raise RuntimeError("missing selected-capture boundary")
fixture = output / "fixture"
run(output, "configure", ["cmake", "-S", source / "tests/generic_primitives/performance",
    "-B", fixture, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCONFIGQML_MEASURE_HANDLES=ON",
    "-DCONFIGQML_SOURCE=" + str(source), "-DCONFIGQML_BUILD=" + str(build),
    *["-DCONFIGQML_" + key + "_SYMBOL=" + value for key, value in symbols.items()]], env=env)
run(output, "fixture-build", ["cmake", "--build", fixture, "--parallel", "2",
    "--target", "layer-structure"], env=env)
path = source / "configcandidate.cpp"
original = path.read_bytes()
before = digest(path)
start = b"        QMap<ConfigPath, ConfigCandidateFact> facts;"
end = b"        return facts;"
if original.count(start) != 1 or original.count(end) != 1:
    raise RuntimeError("nonunique selected capture")
begin, finish = original.index(start), original.index(end)
old = b"""        QMap<ConfigPath, ConfigCandidateFact> facts;
        const auto effective = layers.effective();
        for (const auto &path : paths) {
            ConfigCandidateFact fact;
            fact.effective = valueAt(effective, path).value_or(QVariant());
            fact.maskedBy = layers.info(path).maskedBy;
            fact.targetPresent = targetPresent(base, path, runtime);
            facts.insert(path, fact);
        }
"""
command = [fixture / "layer-structure", "candidateCapturesSelectedFacts"]
try:
    path.write_bytes(original[:begin] + old + original[finish:])
    run(output, "mutated-build", ["cmake", "--build", build, "--parallel", "2",
        "--target", "configqml_core"], env=env)
    text = run(output, "mutated", command, env=env, expected=4)
    require_marker(text, "2 passed, 4 failed, 0 skipped")
    require_marker(text, "provenance_queries= 512")
finally:
    path.write_bytes(original)
    if digest(path) != before:
        raise RuntimeError("selected-capture mutation did not restore bytes")
    run(output, "restored-build", ["cmake", "--build", build, "--parallel", "2",
        "--target", "configqml_core"], env=env)
require_marker(run(output, "restored", command, env=env), "6 passed, 0 failed, 0 skipped")
require_marker(run(output, "parity", [build / "test_generic_primitives",
    "selectedCandidateFactsMatchLayerOracle"], env=env), "3 passed, 0 failed, 0 skipped")
write_json(output / "result.json", {"source_sha256": before, "restored_sha256": digest(path),
    "full_provenance_mutation_rejected": True})
print("SELECTED_FACTS_MUTATION_OK")
