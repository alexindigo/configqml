#!/usr/bin/env python3
"""Guest-only control: ignoring allowed leaves must fail the cache oracles."""
import argparse
from pathlib import Path

from harness import digest, guest_environment, require_marker, run, write_json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
env = guest_environment()
env["QT_FORCE_STDERR_LOGGING"] = "1"
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
source = args.source.resolve() / "confighandle.cpp"
original = source.read_bytes()
before = digest(source)
needle = b"key.allowed = childSelection.selection().leaves;"
if original.count(needle) != 1:
    raise RuntimeError("nonunique cache identity boundary")
build = args.build.resolve()
cases = [("test_generic_primitives_handles", "cachedLookupRespectsSelection"),
         ("test_qml_generic_primitives_handles", "cachedSelectionBindings")]
targets = [test for test, _ in cases]
try:
    source.write_bytes(original.replace(needle, b"key.allowed.clear();"))
    run(output, "mutated-build", ["cmake", "--build", build, "--parallel", "2", "--target"]
        + targets, env=env)
    for test, case in cases:
        text = run(output, "mutated-" + test, [build / test, case], env=env, expected=4)
        require_marker(text, "4 failed, 0 skipped")
        require_marker(text, case)
finally:
    source.write_bytes(original)
    if digest(source) != before:
        raise RuntimeError("mutation failed to restore source")
    run(output, "restored-build", ["cmake", "--build", build, "--parallel", "2", "--target"]
        + targets, env=env)
for test, case in cases:
    text = run(output, "restored-" + test, [build / test, case], env=env)
    require_marker(text, "6 passed, 0 failed, 0 skipped")
write_json(output / "result.json", dict(source_sha256=before, restored_sha256=digest(source),
    native_and_qml_rejected=True, restored_passed=True))
print("HANDLE_CACHE_MUTATION_OK")
