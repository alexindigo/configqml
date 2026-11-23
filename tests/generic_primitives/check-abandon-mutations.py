#!/usr/bin/env python3
"""Guest-only controls: each abandon mutation must be rejected by its oracle."""
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
build = args.build.resolve()
test = "test_generic_primitives_abandon"
controls = [
    # Abandoning by key (every pending intent) instead of by the Apply's own
    # inputs must lose the newer change and the other writer's value.
    ("abandon-by-key", "configapply.cpp",
     b"if (!inputs.contains(intent.input) || !intent.reservation.isEmpty())",
     b"if (!intent.reservation.isEmpty())",
     "newerChangesAndOtherWritersSurvive", "f.store->value({\"settings\", \"n\"}).toInt()"),
    # Dropping the intent without replaying the remaining state must leave
    # readers on the discarded value.
    ("no-reader-restore", "configapply.cpp",
     b"    *m_layers = std::move(candidate);\n    m_intents = std::move(intents);",
     b"    m_intents = std::move(intents);",
     "failedApplyAbandonRestoresSaved", "valueAtCallback"),
]
receipts = []
for name, filename, needle, replacement, case, marker in controls:
    source = args.source.resolve() / filename
    original = source.read_bytes()
    if original.count(needle) != 1:
        raise RuntimeError("nonunique mutation boundary: " + name)
    before = digest(source)
    try:
        source.write_bytes(original.replace(needle, replacement))
        run(output, name + "-build", ["cmake", "--build", build, "--parallel", "2", "--target", test],
            env=env)
        red = run(output, name, [build / test, case], env=env, expected=1)
        require_marker(red, marker)
    finally:
        source.write_bytes(original)
        if digest(source) != before:
            raise RuntimeError("mutation did not restore source: " + name)
        run(output, name + "-restored-build", ["cmake", "--build", build, "--parallel", "2",
                                              "--target", test], env=env)
    green = run(output, name + "-restored", [build / test, case], env=env)
    require_marker(green, "0 failed, 0 skipped")
    receipts.append(dict(control=name, source_sha256=before, restored_sha256=digest(source),
                         mutation_rejected=True, restored_passed=True))
write_json(output / "result.json", receipts)
print("ABANDON_MUTATIONS_OK")
