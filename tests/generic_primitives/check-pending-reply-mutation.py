#!/usr/bin/env python3
"""Guest-only negative control: every requested exact reader must acknowledge."""
import argparse
from pathlib import Path

from harness import digest, guest_environment, require_marker, run, write_json

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
env = guest_environment()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
source = args.source.resolve() / "configpendingreply.cpp"
original = source.read_bytes()
original_hash = digest(source)
needle = b"if (--outstanding == 0)"
if original.count(needle) != 1:
    raise RuntimeError("exact-reader completion control is not unique")
build = args.build.resolve()
test = build / "test_generic_primitives"
try:
    source.write_bytes(original.replace(needle, b"if (true) // NEGATIVE_CONTROL_FIRST_READER"))
    run(output, "bypass-build", ["cmake", "--build", build, "--parallel", "2",
                                 "--target", "test_generic_primitives"], env=env)
    red = run(output, "bypass", [test, "pendingReadComposesExactViews:unchanged"],
              env=env, expected=1)
    require_marker(red, "FAIL!")
    require_marker(red, "'!reply->isFinished()' returned FALSE")
finally:
    source.write_bytes(original)
    if digest(source) != original_hash:
        raise RuntimeError("negative control failed to restore source")
    run(output, "restored-build", ["cmake", "--build", build, "--parallel", "2",
                                   "--target", "test_generic_primitives"], env=env)
green = run(output, "restored", [test, "pendingReadComposesExactViews"], env=env)
require_marker(green, "0 failed, 0 skipped")
write_json(output / "result.json", {"source_sha256": original_hash,
                                    "restored_sha256": digest(source),
                                    "bypass_rejected": True, "restored_passed": True})
print("PENDING_REPLY_MUTATION_OK")
