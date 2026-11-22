#!/usr/bin/env python3
"""Guest-only control: an atomic slot does not certify every logical field."""
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
source = args.source.resolve() / "configobservation.cpp"
original = source.read_bytes()
needle = b"if (!have.targetIdentity.isEmpty() && need.logicalPaths) {"
if original.count(needle) != 1:
    raise RuntimeError("logical coverage boundary is not unique")
before = digest(source)
build = args.build.resolve()
test = build / "test_generic_primitives"
try:
    source.write_bytes(original.replace(needle,
        b"if (false && !have.targetIdentity.isEmpty() && need.logicalPaths) {"))
    run(output, "atomic-only-build", ["cmake", "--build", build, "--parallel", "2",
                                      "--target", "test_generic_primitives"], env=env)
    red = run(output, "atomic-only", [test, "composedSelectedCoverage"], env=env, expected=4)
    require_marker(red, "2 passed, 4 failed, 0 skipped")
    require_marker(red, "!ConfigObservationAccess::applicable(b.observation(), a.observation())")
finally:
    source.write_bytes(original)
    if digest(source) != before:
        raise RuntimeError("logical coverage mutation did not restore source")
    run(output, "restored-build", ["cmake", "--build", build, "--parallel", "2",
                                    "--target", "test_generic_primitives"], env=env)
green = run(output, "restored", [test, "composedSelectedCoverage"], env=env)
require_marker(green, "6 passed, 0 failed, 0 skipped")
write_json(output / "result.json", {"source_sha256": before,
    "restored_sha256": digest(source), "atomic_only_rejected": True, "restored_passed": True})
print("LOGICAL_SELECTION_MUTATION_OK")
