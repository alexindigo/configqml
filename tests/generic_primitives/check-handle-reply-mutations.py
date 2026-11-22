#!/usr/bin/env python3
"""Guest-only controls: each mutation must be rejected by the handle oracles."""
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
native = ("test_generic_primitives_handles", "editResetDiscardApply")
sketches = ("test_qml_generic_primitives_handles", "sketches")
controls = [
    # Completing without the requested readers must fail the composed reply
    # oracles, natively and in the held-runtime QML sketch.
    ("reader-composition-bypass", "configpendingreply.cpp",
     b"void ConfigPendingReplyState::observeViews() {\n    outstanding = views.size() - 1;",
     b"void ConfigPendingReplyState::observeViews() {\n    outstanding = 0;",
     [native + ("GenericHandlesTest::editResetDiscardApply()",),
      sketches + ("QmlGenericHandlesTest::sketches()",)]),
    # A value notification on metadata-only publications (equal intent,
    # confirmations) must fail the exactly-once binding oracle.
    ("notify-on-metadata", "confighandle.cpp",
     b"if (handle && !guard.invoke([&] { emit handle->stateChanged(); }))",
     b"if (handle && !guard.invoke([&] { emit handle->valueChanged(); emit handle->stateChanged(); }))",
     [("test_generic_primitives_handles", "generatedLookupAndBindings",
       "GenericHandlesTest::generatedLookupAndBindings()")]),
    # Without narrowing, the exact selected reader cannot certify the write.
    ("unnarrowed-write-coverage", "confighandle.cpp",
     b"narrowed.observation =\n                        ConfigObservationAccess::narrow(narrowed.observation, leaves);",
     b"narrowed.observation = narrowed.observation;",
     [("test_generic_primitives_handles", "liveVerbsComposeReplies",
       "GenericHandlesTest::liveVerbsComposeReplies()")]),
]
receipts = []
for name, filename, needle, replacement, cases in controls:
    source = args.source.resolve() / filename
    original = source.read_bytes()
    if original.count(needle) != 1:
        raise RuntimeError("nonunique mutation boundary: " + name)
    before = digest(source)
    targets = sorted({case[0] for case in cases})
    try:
        source.write_bytes(original.replace(needle, replacement))
        run(output, name + "-build", ["cmake", "--build", build, "--parallel", "2", "--target"]
            + targets, env=env)
        for test, case, marker in cases:
            red = run(output, name + "-" + test, [build / test, case], env=env, expected=1)
            require_marker(red, marker)
    finally:
        source.write_bytes(original)
        if digest(source) != before:
            raise RuntimeError("mutation did not restore source: " + name)
        run(output, name + "-restored-build", ["cmake", "--build", build, "--parallel", "2",
                                              "--target"] + targets, env=env)
    for test, case, _ in cases:
        green = run(output, name + "-" + test + "-restored", [build / test, case], env=env)
        require_marker(green, "0 failed, 0 skipped")
    receipts.append(dict(control=name, source_sha256=before, restored_sha256=digest(source),
                         mutation_rejected=True, restored_passed=True))
write_json(output / "result.json", receipts)
print("HANDLE_REPLY_MUTATIONS_OK")
