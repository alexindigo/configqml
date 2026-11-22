#!/usr/bin/env python3
"""Reject the old per-leaf namespace reconstruction on the actual Core DSO."""
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
symbols = [line.split()[0] for line in names.splitlines() if "putValue" in line.split()[0]]
if len(symbols) != 1:
    raise RuntimeError("nonunique actual per-leaf write boundary")
fixture = output / "fixture-build"
run(output, "configure", ["cmake", "-S", source / "tests/generic_primitives/performance",
    "-B", fixture, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCONFIGQML_MEASURE_HANDLES=ON",
    "-DCONFIGQML_PUT_VALUE_SYMBOL=" + symbols[0], "-DCONFIGQML_SOURCE=" + str(source),
    "-DCONFIGQML_BUILD=" + str(build)], env=env)
run(output, "fixture-build", ["cmake", "--build", fixture, "--parallel", "2",
    "--target", "layer-structure"], env=env)
path = source / "configlayers.cpp"
original = path.read_bytes()
before = digest(path)
start = b"QVariantMap ConfigLayers::effective() const {"
end = b"std::optional<QVariant> ConfigLayers::value(const ConfigPath &path) const {"
if original.count(start) != 1 or original.count(end) != 1:
    raise RuntimeError("nonunique effective-tree implementation")
begin, finish = original.index(start), original.index(end)
old = b"""QVariantMap ConfigLayers::effective() const {
    if (!valid())
        return {};
    auto out = m_schema.defaults();
    for (const auto &layer : m_layers)
        for (const auto &source : layer.sources)
            if (participates(source))
                for (auto it = source.admitted.cbegin(); it != source.admitted.cend(); ++it)
                    putValue(out, it.key(), it.value().value);
    return out;
}
"""
try:
    path.write_bytes(original[:begin] + old + original[finish:])
    run(output, "mutated-build", ["cmake", "--build", build, "--parallel", "2",
        "--target", "configqml_core"], env=env)
    text = run(output, "mutated", [fixture / "layer-structure"], env=env, expected=4)
    require_marker(text, "3 passed, 4 failed, 0 skipped")
    require_marker(text, "individual_writes= 512")
finally:
    path.write_bytes(original)
    if digest(path) != before:
        raise RuntimeError("mutation did not restore production bytes")
    run(output, "restored-build", ["cmake", "--build", build, "--parallel", "2",
        "--target", "configqml_core"], env=env)
text = run(output, "restored", [fixture / "layer-structure"], env=env)
require_marker(text, "7 passed, 0 failed, 0 skipped")
require_marker(text, "individual_writes= 0")
write_json(output / "result.json", {"source_sha256": before, "restored_sha256": digest(path),
    "old_reconstruction_rejected": True, "typed_atomic_and_opaque_trees_preserved": True})
print("EFFECTIVE_ASSEMBLY_MUTATION_OK")
