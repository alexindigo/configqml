#!/usr/bin/env python3
"""Guest-only controls for selected facts, exact request IDs and pending paths."""
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
test = build / "test_generic_primitives"
controls = [
    ("unselected-capture", "configreadprojection.cpp",
     b"installed = ConfigObservationAccess::select(capture.snapshot, paths, prefix);",
     b"installed = capture.snapshot;", "composedSelectedCoverage", 4,
     "4 failed, 0 skipped"),
    ("cross-view-request", "configreadprojection.cpp",
     b"if (capture.projectionId != state.id)\n                        capture.requestId = 0;",
     b"if (false && capture.projectionId != state.id)\n                        capture.requestId = 0;",
     "composedSelectedCoverage", 2, "!bCalled"),
    ("relative-edit-key", "configobservation.cpp", b"const auto edit = edits.constFind(path);",
     b"const auto edit = edits.constFind(relative);", "selectedProjectionTracksDraftCandidates",
     1, "frozen->selectedState({\"a\"}).pending()"),
]
receipts = []
for name, filename, needle, replacement, case, expected, marker in controls:
    source = args.source.resolve() / filename
    original = source.read_bytes()
    if original.count(needle) != 1:
        raise RuntimeError("nonunique mutation boundary: " + name)
    before = digest(source)
    try:
        source.write_bytes(original.replace(needle, replacement))
        run(output, name + "-build", ["cmake", "--build", build, "--parallel", "2",
                                     "--target", "test_generic_primitives"], env=env)
        red = run(output, name, [test, case], env=env, expected=expected)
        require_marker(red, marker)
    finally:
        source.write_bytes(original)
        if digest(source) != before:
            raise RuntimeError("mutation did not restore source: " + name)
        run(output, name + "-restored-build", ["cmake", "--build", build, "--parallel", "2",
                                              "--target", "test_generic_primitives"], env=env)
    green = run(output, name + "-restored", [test, case], env=env)
    require_marker(green, "0 failed, 0 skipped")
    receipts.append(dict(control=name, source_sha256=before, restored_sha256=digest(source),
                         mutation_rejected=True, restored_passed=True))
write_json(output / "result.json", receipts)
print("SELECTED_PROJECTION_MUTATIONS_OK")
