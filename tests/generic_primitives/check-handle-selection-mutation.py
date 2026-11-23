#!/usr/bin/env python3
"""Guest-only negative control: generated lookup cannot widen its selection."""
import argparse
from pathlib import Path

from harness import digest, guest_environment, require_marker, run, write_json

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
env = guest_environment()
env["QT_LOGGING_TO_CONSOLE"] = "1"
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
source = args.source.resolve() / "confighandle.cpp"
original = source.read_bytes()
original_hash = digest(source)
needle = b"ConfigSelection::capture(m_selection.schema, absolute, m_selection.leaves,"
if original.count(needle) != 1:
    raise RuntimeError("selected-handle boundary is not unique")
build = args.build.resolve()
test = build / "test_generic_primitives"
try:
    source.write_bytes(original.replace(needle,
        b"ConfigSelection::capture(m_selection.schema, absolute, m_selection.schema.leaves(),"))
    run(output, "widened-build", ["cmake", "--build", build, "--parallel", "2",
                                  "--target", "test_generic_primitives"], env=env)
    red = run(output, "widened", [test, "generatedSelectionNeverWidens"], env=env, expected=1)
    require_marker(red, "FAIL!")
    require_marker(red, "generatedSelectionNeverWidens")
finally:
    source.write_bytes(original)
    if digest(source) != original_hash:
        raise RuntimeError("negative control failed to restore source")
    run(output, "restored-build", ["cmake", "--build", build, "--parallel", "2",
                                   "--target", "test_generic_primitives"], env=env)
green = run(output, "restored", [test, "generatedSelectionUsesDeclarations",
    "generatedSelectionNeverWidens", "generatedSelectionOwnsBorrowedPaths"], env=env)
require_marker(green, "5 passed, 0 failed, 0 skipped")
write_json(output / "result.json", {"source_sha256": original_hash,
    "restored_sha256": digest(source), "widening_rejected": True, "restored_passed": True})
print("HANDLE_SELECTION_MUTATION_OK")
