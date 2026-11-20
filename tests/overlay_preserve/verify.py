#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Installed-prefix/old-client/ABI proof. Run only in an authorized project VM.

Prerequisites: the caller installs prefix-base and prefix-candidate and retains
consumer-base/{layout,overlay-preserve,import-only}, built against prefix-base
before candidate installation. Never rebuild those retained baseline witnesses
against the candidate. This driver builds only the new candidate consumers.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def run(*command, env=None):
    result = subprocess.run(command, text=True, capture_output=True, env=env, check=True)
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    return result.stdout


def exports(library):
    output = subprocess.check_output(["nm", "-D", "--defined-only", str(library)], text=True)
    return {line.split()[-1] for line in output.splitlines()}


def main():
    workspace = Path(sys.argv[1]).resolve()
    assert not workspace.is_relative_to(Path("/tmp"))
    assert Path(os.environ["TMPDIR"]).resolve().is_relative_to(workspace)
    subprocess.run(["systemd-detect-virt", "--vm"], check=True)
    baseline = workspace / "prefix-base"
    candidate = workspace / "prefix-candidate"
    for name in ("layout", "overlay-preserve", "import-only"):
        binary = workspace / "consumer-base" / name
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise RuntimeError(f"missing retained baseline consumer: {binary}")
    output = workspace / "artifacts" / "installed-abi"
    output.mkdir(exist_ok=False)
    environment = os.environ.copy()
    environment.update(LD_LIBRARY_PATH=str(candidate / "lib"),
                       QML_IMPORT_PATH=str(candidate / "lib/qt6/qml"),
                       QML2_IMPORT_PATH=str(candidate / "lib/qt6/qml"))
    run("cmake", "-S", str(workspace / "source/tests/overlay_preserve"),
        "-B", str(workspace / "consumer-candidate"), "-G", "Unix Makefiles",
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={candidate}")
    run("cmake", "--build", str(workspace / "consumer-candidate"), "--parallel", "2")
    layouts = {}
    for name in ("base", "candidate"):
        env = environment.copy()
        env["LD_LIBRARY_PATH"] = str(workspace / f"prefix-{name}" / "lib")
        layouts[name] = run(str(workspace / f"consumer-{name}/layout"), env=env)
    assert layouts["base"] == layouts["candidate"], layouts
    (output / "layouts.json").write_text(json.dumps(layouts, indent=2) + "\n")
    header_hashes = {}
    for old in sorted((baseline / "include/configqml").glob("*.h")):
        new = candidate / "include/configqml" / old.name
        assert old.read_bytes() == new.read_bytes(), old.name
        header_hashes[old.name] = hashlib.sha256(new.read_bytes()).hexdigest()
    assert header_hashes
    symbols = {}
    for relative in ("lib/libconfigqml_core.so.0.5", "lib/libconfigqml.so.0.5",
                     "lib/qt6/qml/ConfigQml/libconfigqmlplugin.so"):
        before, after = exports(baseline / relative), exports(candidate / relative)
        removed = sorted(before - after)
        assert not removed, (relative, removed)
        symbols[relative] = {"removed": removed, "added": sorted(after - before)}
    for library in ("libconfigqml_core.so.0.5", "libconfigqml.so.0.5"):
        dynamic = run("readelf", "-d", str(candidate / "lib" / library))
        assert f"[{library}]" in dynamic
    core_links = run("ldd", str(candidate / "lib/libconfigqml_core.so.0.5"))
    assert "libQt6Core.so" in core_links
    assert all("libQt6Core.so" in line for line in core_links.splitlines() if "libQt6" in line)
    for client in ("base", "candidate"):
        native = run(str(workspace / f"consumer-{client}/overlay-preserve"), env=environment)
        assert "0 failed" in native
        (output / f"native-{client}.log").write_text(native)
        directory = output / f"qml-{client}"
        for mode in ("write", "cold"):
            qml = run(str(workspace / f"consumer-{client}/import-only"),
                      str(workspace / "source/tests/overlay_preserve/consumer.qml"),
                      str(directory), mode, env=environment)
            assert "PASS" in qml
    result = {"headers": header_hashes, "symbols": symbols, "layouts": layouts,
              "old_native": "PASS", "installed_native": "PASS",
              "old_import_only": "PASS", "installed_import_only": "PASS"}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("OVERLAY_PRESERVE_INSTALLED_ABI: PASS")


if __name__ == "__main__":
    main()
