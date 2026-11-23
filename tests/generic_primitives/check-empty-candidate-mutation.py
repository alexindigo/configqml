#!/usr/bin/env python3
"""Reject empty-batch source readmission, then restore exact production bytes."""
import argparse
from pathlib import Path

from harness import digest, guest_environment, require_marker, run, write_json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
path = args.source.resolve() / "configcandidate.cpp"
build = args.build.resolve()
original = path.read_bytes()
before = digest(path)
guard = b"    if (!edits.isEmpty()) {"
if original.count(guard) != 1:
    raise RuntimeError("nonunique empty-candidate admission guard")
env = guest_environment()
env["QT_FORCE_STDERR_LOGGING"] = "1"
command = [build / "test_generic_primitives", "emptyCandidateUsesAdmittedState"]
try:
    path.write_bytes(original.replace(guard, b"    if (true) {"))
    run(output, "mutated-build", ["cmake", "--build", build, "--parallel", "2",
        "--target", "configqml_core"], env=env)
    require_marker(run(output, "mutated", command, env=env, expected=3),
                   "2 passed, 3 failed, 0 skipped")
finally:
    path.write_bytes(original)
    if digest(path) != before:
        raise RuntimeError("empty-candidate mutation did not restore source")
    run(output, "restored-build", ["cmake", "--build", build, "--parallel", "2",
        "--target", "configqml_core"], env=env)
require_marker(run(output, "restored", command, env=env), "5 passed, 0 failed, 0 skipped")
write_json(output / "result.json", {"source_sha256": before, "restored_sha256": digest(path),
    "empty_readmission_rejected": True})
print("EMPTY_CANDIDATE_MUTATION_OK")
