#!/usr/bin/env python3
"""Check frozen old binaries, public layouts/metadata and installed exports."""

import argparse
import json
from pathlib import Path

from arch_compatibility import matched_arch_lane
from harness import digest, guest_environment, installed_manifest, loader, require_marker, run, write_json
from weak_exports import audit_consumers, classify_losses, control_checks, symbols


def metadata_compatible(before, after):
    for name, old in before.items():
        new = after[name]
        for category in ("methods", "properties", "enums"):
            for entry in old[category]:
                index = entry["index"]
                if index >= len(new[category]) or entry != new[category][index]:
                    raise RuntimeError(f"released Qt metadata changed: {name}/{category}/{index}")


def check_lane(baseline, source, prefix, output, consumers=(), roots=()):
    env = guest_environment(prefix)
    original = json.loads((baseline / "baseline.json").read_text())
    control_checks()
    imports = audit_consumers(output,
        [baseline / path for path in original["binaries"].values()] + list(consumers),
        [prefix, Path("/usr/bin"), Path("/usr/lib/qt6/qml")] + list(roots),
        env, run, digest, write_json)
    for path, expected in original["frozen_files"].items():
        if digest(baseline / path) != expected:
            raise RuntimeError("frozen baseline input changed: " + path)
    for name, marker in (("old-native", "OLD_NATIVE_OK"), ("old-metaobject", "OLD_METAOBJECT_OK")):
        binary = baseline / original["binaries"][name]
        loader(output, name + "-loader", binary, prefix, env, core_only=name == "old-native")
        require_marker(run(output, name, [binary], env=env), marker)
    qml = baseline / original["binaries"]["old-qml"]
    loader(output, "old-qml-linkage", qml, prefix, env, plugin_only=True)
    text = run(output, "old-qml", [qml, prefix / "lib/qt6/qml", baseline / "old-client.qml"], env=env)
    require_marker(text, "OLD_QML_OK")
    origins = [line for line in text.splitlines() if line.startswith("OLD_QML_LOADER ")]
    if not any("libconfigqmlplugin.so" in line for line in origins) or any(
            str(prefix) + "/lib/" not in line for line in origins):
        raise RuntimeError("old QtQml client did not exclusively load the installed candidate")
    build = output / "layouts-build"
    run(output, "layouts-configure", ["cmake", "-S", source / "tests/generic_primitives/compat",
        "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={prefix}",
        f"-DCONFIGQML_LAYOUT_SOURCE={baseline / 'public-layouts.cpp'}"], env=env)
    run(output, "layouts-build", ["cmake", "--build", build, "--target", "public-layouts",
        "--parallel", "2"], env=env)
    current = json.loads(run(output, "public-layouts", [build / "public-layouts"], env=env))
    previous = json.loads((baseline / "public-abi.json").read_text())
    if previous["layouts"] != current["layouts"]:
        write_json(output / "layout-delta.json", {"before": previous["layouts"], "after": current["layouts"]})
        raise RuntimeError("released public layouts changed")
    metadata_compatible(previous["metadata"], current["metadata"])
    installed = installed_manifest(prefix, output, env)
    missing, allowed = {}, {}
    for name, old in original["installation"]["libraries"].items():
        library = Path(old["path"])
        if digest(library) != old["sha256"]:
            raise RuntimeError("frozen baseline library changed: " + str(library))
        before = symbols(run(output, name + "-baseline-symbol-kinds",
            ["nm", "-D", "--defined-only", "--format=posix", library], env=env))
        if set(before) != set(old["symbols"]):
            raise RuntimeError("baseline symbol inventory changed")
        accepted, rejected = classify_losses(before, installed["libraries"][name]["symbols"], imports)
        if accepted:
            allowed[name] = accepted
        if rejected:
            missing[name] = rejected
    write_json(output / "symbols-reviewed-losses.json", allowed)
    write_json(output / "symbols-missing.json", missing)
    if missing:
        raise RuntimeError("baseline exports missing; see symbols-missing.json")
    for path, expected in original["frozen_files"].items():
        if digest(baseline / path) != expected:
            raise RuntimeError("old consumer hash changed during qualification: " + path)
    write_json(output / "qualification.json", {"boundary": "slice", "installation": installed,
        "reviewed_weak_losses": allowed,
        "layouts": len(current["layouts"]), "metaobjects": len(current["metadata"]),
        "baseline_manifest_sha256": digest(baseline / "baseline.json"),
        "additive_headers": sorted(set(installed["headers"]) - set(original["installation"]["headers"]))})
    return json.loads((output / "qualification.json").read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("baseline", "candidate-source", "candidate-prefix", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--slice", action="store_true")
    parser.add_argument("--consumer", action="append", type=Path, default=[])
    parser.add_argument("--consumer-root", action="append", type=Path, default=[])
    args = parser.parse_args()
    baseline, source, prefix, output = (getattr(args, name).resolve() for name in
        ("baseline", "candidate_source", "candidate_prefix", "output"))
    plain = check_lane(baseline, source, prefix, output, args.consumer, args.consumer_root)
    if not args.slice:
        arch = matched_arch_lane(baseline, source, output / "arch", check_lane,
                                 args.consumer, args.consumer_root)
        write_json(output / "qualification.json", {
            "boundary": "full", "plain": plain, "arch": arch})
    print("GENERIC_COMPATIBILITY_OK " + ("slice" if args.slice else "full"))


if __name__ == "__main__":
    main()
