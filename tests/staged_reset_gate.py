#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Guest-only installed-consumer witnesses for staged override removal."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import subprocess
import sys


def run(command, output, env=None):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env, check=False)
    output.write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {command}; see {output}")
    return result.stdout


def digest(path):
    return hashlib.file_digest(path.open("rb"), "sha256").hexdigest()


def manifest(root):
    return {str(p.relative_to(root)): digest(p) for p in sorted(root.rglob("*"))
            if p.is_file() and ".git" not in p.parts and p.name != ".envrc"}


def validate(profile):
    data = json.loads(profile.read_text())
    if data["name"] != "configqml-staged-override-removal":
        raise RuntimeError("wrong guest profile")
    if pwd.getpwuid(os.getuid()).pw_name != "tester":
        raise RuntimeError("requires tester inside the named guest")
    uuid = subprocess.check_output(["sudo", "cat", "/sys/class/dmi/id/product_uuid"],
                                   text=True).strip()
    if uuid != data["uuid"]:
        raise RuntimeError("guest DMI UUID does not match the fork receipt")
    workspace = Path(data["workspace"]).resolve(strict=True)
    if workspace != Path("/home/tester/Projects/configqml/staged-override-removal"):
        raise RuntimeError("wrong guest workspace")
    if Path(os.environ.get("TMPDIR", "")).resolve() != workspace / "tmp":
        raise RuntimeError("TMPDIR must be the disk-backed task tmp directory")
    filesystem = subprocess.check_output(["findmnt", "-T", str(workspace), "-n", "-o", "FSTYPE"],
                                         text=True).strip()
    if filesystem in ("tmpfs", "ramfs") or not filesystem:
        raise RuntimeError("workspace must be disk-backed")
    return workspace


def installed_environment(prefix):
    env = dict(os.environ)
    for key in ("QML2_IMPORT_PATH", "CMAKE_PREFIX_PATH", "configqml_DIR", "LD_PRELOAD"):
        env.pop(key, None)
    qt = "/usr/lib/qt6"
    env.update(LD_LIBRARY_PATH=f"{prefix}/lib:/usr/lib",
               QML_IMPORT_PATH=f"{prefix}/lib/qt6/qml:{qt}/qml",
               QT_PLUGIN_PATH=f"{qt}/plugins", QT_QPA_PLATFORM="offscreen",
               QSG_RHI_BACKEND="software", QT_FATAL_WARNINGS="1")
    return env


def build_consumers(args, out):
    build = out / "clients"
    run(["cmake", "-S", str(args.source / "tests/staged_reset"), "-B", str(build),
         "-G", "Unix Makefiles", f"-Dconfigqml_DIR={args.prefix}/lib/cmake/configqml",
         f"-DSTAGED_RESET_NEW={'ON' if args.mode == 'installed' else 'OFF'}"],
        out / "clients-configure.log")
    run(["cmake", "--build", str(build), "--parallel", "2"], out / "clients-build.log")
    return build


def focused_contract(args, out):
    listing = run(["ctest", "--test-dir", str(args.build), "--show-only=json-v1"],
                  out / "ctest-list.json")
    names = {entry["name"] for entry in json.loads(listing)["tests"]}
    required = {"draft_reset", "qml_draft_reset", "draft_reset_qml_lint"}
    if not required.issubset(names):
        raise RuntimeError(f"missing staged-reset CTest entries: {required - names}")
    run(["ctest", "--test-dir", str(args.build), "--output-on-failure", "--no-tests=error",
         "-R", "^(draft_reset|qml_draft_reset|draft_reset_qml_lint)$"], out / "focused.log")


def baseline(args, out):
    expected = json.loads((args.baseline / "source-manifest.json").read_text())
    profile = json.loads(args.profile.read_text())
    actual = manifest(args.baseline)
    actual.pop("source-manifest.json", None)
    if (actual != expected["files"] or expected["tag"] != "v0.5.1" or
            expected["commit"] != profile["baseline_commit"]):
        raise RuntimeError("baseline source hashes do not match the delivered tag manifest")
    build = build_consumers(args, out)
    env = installed_environment(args.prefix)
    native = out / "native.json"
    native.write_text('{"n":9}')
    text = run([str(build / "old-native"), "baseline", str(native)],
               out / "native.log", env)
    assert "OLD_NATIVE_BASELINE: DEFAULT_ASSIGNMENT_NOT_REMOVAL" in text
    text = run([str(build / "old-native"), "cold", str(native)], out / "cold.log", env)
    assert "OLD_NATIVE_COLD: EXPLICIT_DEFAULT_STILL_PRESENT" in text
    (out / "qml.json").write_text('{"n":9}')
    text = run([str(build / "import-only"), str(args.source / "tests/staged_reset/baseline.qml"),
                str(out)], out / "qml.log", env)
    assert "OLD_QML_BASELINE: DRAFT_RESET_REFUSED" in text
    assert "OLD_QML_BASELINE: DEFAULT_ASSIGNMENT_NOT_REMOVAL" in text
    assert json.loads((out / "qml.json").read_text()) == {"n": 1}
    links = run(["ldd", str(build / "import-only")], out / "import-linkage.log", env)
    assert "libconfigqml" not in links
    links = run(["ldd", str(build / "old-native")], out / "native-linkage.log", env)
    assert str(args.prefix / "lib/libconfigqml_core.so.0.5") in links
    run([str(build / "abi-layout")], out / "old-layout.txt", env)
    shutil.copyfile(args.source / "tests/staged_reset/baseline.qml", out / "baseline.qml")
    (out / "manifest.json").write_text(json.dumps({
        "baseline": expected, "fixtures": manifest(args.source / "tests/staged_reset"),
        "libraries": manifest(args.prefix), "clients": manifest(build),
    }, indent=2) + "\n")
    artifacts = [build / name for name in ("old-native", "import-only", "abi-layout")]
    artifacts += [out / "old-layout.txt", out / "baseline.qml", out / "manifest.json"]
    (Path(profile["workspace"]) / "baseline-consumers.json").write_text(json.dumps({
        "baseline_commit": expected["commit"],
        "files": {str(p): digest(p) for p in artifacts}, "root": str(out),
    }, indent=2) + "\n")
    print("STAGED_RESET_BASELINE: EXPECTED_CONTRACT_FAILURES_VERIFIED")


def installed(args, out):
    focused_contract(args, out)
    build = build_consumers(args, out)
    env = installed_environment(args.prefix)
    native = out / "native.json"
    native.write_text('{"n":9}')
    text = run([str(build / "new-native"), "apply", str(native)], out / "native.log", env)
    assert "NEW_NATIVE_INSTALLED: STAGED_DISCARD_APPLY_RAW_REMOVAL" in text
    text = run([str(build / "new-native"), "cold", str(native)], out / "native-cold.log", env)
    assert "NEW_NATIVE_COLD: LEGACY_7" in text
    assert "n" not in json.loads(native.read_text())
    (out / "user.json").write_text('{"n":9}')
    (out / "legacy-reset.json").write_text('{"n":7,"other":8}')
    for mode in ("apply", "cold"):
        run([str(build / "import-only"), str(args.source / "tests/tst_draft_reset.qml"),
             str(out), mode], out / f"qml-{mode}.log", env)
    assert json.loads((out / "user.json").read_text()) == {}
    (out / "shapes-user.json").write_text(json.dumps({
        "flag": True, "zero": 9, "text": "chosen", "list": ["chosen"],
        "record": {"label": "chosen", "opaque": True}, "emptyRecord": {"opaque": True},
        "map": {"chosen": "chosen"}, "nullable": "chosen", "optional": "chosen",
        "outsideUnknown": False,
    }))
    (out / "shapes-legacy.json").write_text(json.dumps({
        "flag": False, "zero": 0, "text": "", "list": [],
        "record": {"label": "lower", "wide": 18446744073709551615,
                   "__proto__": {"x": False}, "constructor": "own"},
        "emptyRecord": {}, "map": {}, "nullable": None,
    }))
    for mode in ("apply", "cold"):
        run([str(build / "import-only"), str(args.source / "tests/staged_reset/shapes.qml"),
             str(out), mode], out / f"qml-shapes-{mode}.log", env)
    assert json.loads((out / "shapes-user.json").read_text()) == {"outsideUnknown": False}
    links = run(["ldd", str(build / "new-native")], out / "native-linkage.log", env)
    assert str(args.prefix / "lib/libconfigqml_core.so.0.5") in links
    assert not re.search(r"libQt6(?:Qml|Quick|Gui|Widgets|DBus|Concurrent)", links)
    (out / "manifest.json").write_text(json.dumps({
        "fixtures": manifest(args.source / "tests/staged_reset"),
        "libraries": manifest(args.prefix), "clients": manifest(build),
        "qml_fixture": digest(args.source / "tests/tst_draft_reset.qml"),
        "source": json.loads((args.source / "delivery-manifest.json").read_text()),
    }, indent=2) + "\n")
    print("STAGED_RESET_INSTALLED: PASS")


def abi(args, out):
    profile = json.loads(args.profile.read_text())
    workspace = Path(profile["workspace"])
    receipt = json.loads((workspace / "baseline-consumers.json").read_text())
    assert receipt["baseline_commit"] == profile["baseline_commit"]
    for name, expected in receipt["files"].items():
        path = Path(name).resolve(strict=True)
        assert path.is_relative_to(workspace) and digest(path) == expected
    old = Path(receipt["root"])
    baseline_prefix = workspace / "prefix-base"
    baseline_manifest = json.loads((old / "manifest.json").read_text())
    if manifest(baseline_prefix) != baseline_manifest["libraries"]:
        raise RuntimeError("baseline installed library/header identity changed")
    env = installed_environment(args.prefix)
    build = build_consumers(args, out)
    layout = run([str(build / "abi-layout")], out / "new-layout.txt", env)
    assert layout == (old / "old-layout.txt").read_text()
    changes = {}
    for name, relative in (
        ("core", "lib/libconfigqml_core.so"),
        ("qml", "lib/libconfigqml.so"),
        ("plugin", "lib/qt6/qml/ConfigQml/libconfigqmlplugin.so"),
    ):
        before = run(["nm", "-D", "--defined-only", str(baseline_prefix / relative)],
                     out / f"{name}-old-symbols.txt")
        after = run(["nm", "-D", "--defined-only", str(args.prefix / relative)],
                    out / f"{name}-new-symbols.txt")
        symbols = lambda text: {line.split()[-1] for line in text.splitlines() if line.split()}
        removed = sorted(symbols(before) - symbols(after))
        changes[name] = {"removed": removed, "added": sorted(symbols(after) - symbols(before))}
        (out / "symbol-changes.json").write_text(json.dumps(changes, indent=2) + "\n")
        if removed:
            raise RuntimeError(f"removed {name} exports: {removed}")
        dynamic = run(["readelf", "-d", str(args.prefix / relative)], out / f"{name}-dynamic.txt")
        if name != "plugin":
            soname = "libconfigqml_core.so.0.5" if name == "core" else "libconfigqml.so.0.5"
            assert f"Library soname: [{soname}]" in dynamic
        if name == "core":
            qt = re.findall(r"Shared library: \[(libQt6[^]]+)\]", dynamic)
            assert qt == ["libQt6Core.so.6"]
    path = out / "old-native.json"
    path.write_text('{"n":9}')
    run([str(old / "clients/old-native"), "baseline", str(path)], out / "old-native.log", env)
    run([str(old / "clients/old-native"), "cold", str(path)], out / "old-cold.log", env)
    (out / "qml.json").write_text('{"n":9}')
    text = run([str(old / "clients/import-only"), str(old / "baseline.qml"), str(out)],
               out / "old-qml.log", env)
    assert "OLD_QML_BASELINE: DRAFT_RESET_REFUSED" in text
    assert json.loads((out / "qml.json").read_text()) == {"n": 1}
    links = run(["ldd", str(old / "clients/old-native")], out / "old-runtime-linkage.log", env)
    assert str(args.prefix / "lib/libconfigqml_core.so.0.5") in links
    (out / "manifest.json").write_text(json.dumps({
        "baseline_receipt": receipt, "candidate_libraries": manifest(args.prefix),
        "candidate_clients": manifest(build),
    }, indent=2) + "\n")
    print("STAGED_RESET_ABI: PASS")


def canaries(args, out):
    focused_contract(args, out)
    # Every fault lives only in a disposable guest source copy. The production
    # build has no bypass flags. A compile/setup failure never counts as a red.
    mutation_file = "configdraftpreview.cpp"
    removals = "    for (const auto &path : selected)\n        edits[path] = std::nullopt;"
    guard = ("        if (p.request->lifetime != state->m_lifetime || !*state->m_lifetime ||\n"
             "            p.request->epoch != state->m_epoch || p.generation <= state->m_generation)\n"
             "            continue;\n"
             "        if (p.requestId && p.requestId != state->m_pending)\n"
             "            continue;")
    cases = [
        ("default-assignment", mutation_file, removals,
         "    for (const auto &path : selected)\n        edits[path] = m_schema.descriptor(path)->defaultValue;", "basicRemoval"),
        ("null-assignment", mutation_file, removals,
         "    for (const auto &path : selected)\n        edits[path] = QVariant::fromValue(nullptr);", "basicRemoval"),
        ("store-reset-at-stage", mutation_file, removals,
         "    for (const auto &path : selected) {\n"
         "        m_store->reset(path, m_store->m_userLayerId);\n"
         "        edits[path] = std::nullopt;\n    }", "basicRemoval"),
        ("nullopt-as-absence", mutation_file,
         "? *edit.value() : candidate.value(path).value_or(QVariant());",
         "? *edit.value() : QVariant();", "basicRemoval"),
        ("drop-unknown-sibling", mutation_file, "    auto candidate = base;",
         '    auto candidate = base;\n    candidate.reset({"settings"}, "user");',
         "unknownSiblingSuppressesFallback"),
        ("stale-reply", mutation_file, guard, "        // Deliberately removed stale-delivery fences.",
         "stalePresentationCannotReplaceSetOrDiscard"),
        ("root-publication", "configworker_p.h",
         '        return operation == "saveSelected" || (applyAttempt && applyAttempt->selectedPublication);',
         '        return operation == "saveSelected";', "selectedPublication"),
    ]
    records = []
    for name, file, before, after, consumer in cases:
        root = out / name
        source = root / "source"
        shutil.copytree(args.source, source, ignore=shutil.ignore_patterns(".git", ".envrc"))
        path = source / file
        text = path.read_text()
        anchor = r"\s+".join(re.escape(token) for token in before.split())
        matches = list(re.finditer(anchor, text))
        if len(matches) != 1:
            raise RuntimeError(f"mutation anchor does not match exactly once: {name}")
        match = matches[0]
        path.write_text(text[:match.start()] + after + text[match.end():])
        (root / "source-manifest.json").write_text(json.dumps(manifest(source), indent=2) + "\n")
        build = root / "build"
        run(["cmake", "-S", str(source), "-B", str(build), "-G", "Unix Makefiles",
             "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON"], root / "configure.log")
        run(["cmake", "--build", str(build), "--target", "test_draft_reset", "test_editors",
             "--parallel", "2"], root / "build.log")
        result = subprocess.run([str(build / "test_draft_reset"), consumer], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        (root / "consumer-red.log").write_text(result.stdout)
        if result.returncode != 1 or not re.search(r"FAIL!\s+: DraftResetTest::" + consumer,
                                                   result.stdout):
            raise RuntimeError(f"mutation did not fail its consumer assertion: {name}")
        run([str(build / "test_editors"), "detachedCoherentAndReopened"], root / "no-removal-control.log")
        records.append({"name": name, "consumer": consumer, "exit": result.returncode,
                        "source_manifest": digest(root / "source-manifest.json")})
        (out / "mutations.json").write_text(json.dumps(records, indent=2) + "\n")
    print("STAGED_RESET_CANARIES: PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("baseline", "canaries", "abi", "installed"))
    for name in ("profile", "source", "baseline", "build", "baseline-build", "prefix", "output"):
        parser.add_argument("--" + name, type=Path, required=name in ("profile", "source", "output"))
    args = parser.parse_args()
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.resolve())
    workspace = validate(args.profile)
    for value in vars(args).values():
        if isinstance(value, Path) and not value.resolve().is_relative_to(workspace):
            raise RuntimeError(f"path outside owned guest workspace: {value}")
    args.output.mkdir(parents=True, exist_ok=False)
    if args.mode == "baseline":
        baseline(args, args.output)
    elif args.mode == "installed":
        installed(args, args.output)
    elif args.mode == "abi":
        abi(args, args.output)
    elif args.mode == "canaries":
        canaries(args, args.output)
    else:
        raise RuntimeError(f"{args.mode} gate not implemented yet; never report a stub as PASS")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"STAGED_RESET_GATE: FAIL: {error}", file=sys.stderr)
        sys.exit(1)
