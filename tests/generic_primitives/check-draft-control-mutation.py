#!/usr/bin/env python3
"""Reject rebuilding admitted store state during draft-only control delivery."""
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
path = source / "configworker.cpp"
original = path.read_bytes()
before = digest(path)
boundary = b"return publishPacket(result, false, false);"
if original.count(boundary) != 1:
    raise RuntimeError("nonunique control publication boundary")
try:
    path.write_bytes(original.replace(boundary, b"return publishPacket(result, false, true);"))
    run(output, "mutated-build", ["cmake", "--build", build, "--parallel", "2",
        "--target", "configqml_core"], env=env)
    text = run(output, "mutated", [fixture / "layer-structure", "draftControlReusesAdmittedCut"],
        env=env, expected=4)
    require_marker(text, "2 passed, 4 failed, 0 skipped")
    require_marker(text, "provenance_queries= 512")
finally:
    path.write_bytes(original)
    if digest(path) != before:
        raise RuntimeError("control publication mutation did not restore bytes")
    run(output, "restored-build", ["cmake", "--build", build, "--parallel", "2",
        "--target", "configqml_core"], env=env)
require_marker(run(output, "restored", [fixture / "layer-structure"], env=env),
    "27 passed, 0 failed, 0 skipped")
write_json(output / "result.json", {"source_sha256": before, "restored_sha256": digest(path),
    "full_publication_on_control_rejected": True})
print("DRAFT_CONTROL_MUTATION_OK")
