#!/usr/bin/env python3
"""Exercise the new compatibility slice from installed and relocated prefixes."""

import argparse
from pathlib import Path
import shutil
import tempfile

from harness import guest_environment, installed_manifest, loader, require_marker, run, write_json


def check_lane(source, prefix, output, full):
    env = guest_environment(prefix)
    build = output / "clients-build"
    run(output, "configure", ["cmake", "-S", source / "tests/generic_primitives/installed",
        "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={prefix}",
        f"-DCONFIGQML_IMPORT_ROOT={prefix / 'lib/qt6/qml'}"], env=env)
    run(output, "build", ["cmake", "--build", build, "--parallel", "2"], env=env)
    native, qml = build / "installed-native", build / "installed-plugin"
    loader(output, "native-loader", native, prefix, env, core_only=True)
    require_marker(run(output, "native", [native], env=env), "GENERIC_INSTALLED_NATIVE_OK argv=3")
    loader(output, "qml-linkage", qml, prefix, env, plugin_only=True)
    text = run(output, "qml", [qml, prefix / "lib/qt6/qml",
        source / "tests/generic_primitives/installed/slice.qml"], env=env)
    require_marker(text, "GENERIC_INSTALLED_QML_OK snapshot=3")
    origins = [line for line in text.splitlines() if line.startswith("OLD_QML_LOADER ")]
    if not any("libconfigqmlplugin.so" in line for line in origins) or any(
            str(prefix) + "/lib/" not in line for line in origins):
        raise RuntimeError("QML consumer loaded outside the installed prefix")
    if full:
        # Generated handles and composed replies, from consumers whose schema
        # fields are unknown to the library build.
        handles_native = build / "installed-handles-native"
        handles_qml = build / "installed-handles-plugin"
        loader(output, "handles-native-loader", handles_native, prefix, env, core_only=True)
        require_marker(run(output, "handles-native", [handles_native], env=env),
                       "GENERIC_INSTALLED_HANDLES_NATIVE_OK maxVisible=3")
        loader(output, "handles-qml-linkage", handles_qml, prefix, env, plugin_only=True)
        text = run(output, "handles-qml", [handles_qml, prefix / "lib/qt6/qml",
            source / "tests/generic_primitives/installed/handles.qml"], env=env)
        require_marker(text, "GENERIC_INSTALLED_HANDLES_QML_OK maxVisible=3")
        origins = [line for line in text.splitlines() if line.startswith("OLD_QML_LOADER ")]
        if not any("libconfigqmlplugin.so" in line for line in origins) or any(
                str(prefix) + "/lib/" not in line for line in origins):
            raise RuntimeError("QML handle consumer loaded outside the installed prefix")
    run(output, "lint", ["cmake", "--build", build, "--target", "installed-qml-lint"], env=env)
    return installed_manifest(prefix, output, env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("source", "prefix", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--slice", action="store_true")
    args = parser.parse_args()
    full = not args.slice
    source, prefix, output = (getattr(args, name).resolve() for name in ("source", "prefix", "output"))
    current = check_lane(source, prefix, output / "original", full)
    output.mkdir(parents=True, exist_ok=True)
    # Keep only observations, not a rebuildable copied prefix, after this proof.
    with tempfile.TemporaryDirectory(prefix="relocated-", dir=output) as temporary:
        relocated = Path(temporary) / "prefix"
        shutil.copytree(prefix, relocated, symlinks=True)
        moved = check_lane(source, relocated, output / "relocated", full)
        if {name: library["sha256"] for name, library in current["libraries"].items()} != {
                name: library["sha256"] for name, library in moved["libraries"].items()}:
            raise RuntimeError("relocation changed a candidate library")
        write_json(output / "qualification.json", {"boundary": "full" if full else "slice",
            "original": current, "relocated": moved})
    print("GENERIC_INSTALLED_OK " + ("full" if full else "slice"))


if __name__ == "__main__":
    main()
