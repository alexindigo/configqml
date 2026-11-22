#!/usr/bin/env python3
"""Checked subprocess/evidence helpers for the guest-only qualification runners."""

import hashlib
import json
import os
from pathlib import Path
import subprocess


def digest(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def guest_environment(prefix=None):
    """Refuse host execution and RAM-backed scratch; keep loader inputs explicit."""
    uuid = subprocess.run(["sudo", "cat", "/sys/class/dmi/id/product_uuid"],
                          check=True, text=True, capture_output=True).stdout.strip()
    expected = os.environ.get("CONFIGQML_TEST_VM_UUID")
    if not expected or uuid.lower() != expected.lower():
        # DMI can be root-readable only. The pinned connector exports the verified
        # identity separately; callers must not infer identity from the hostname.
        raise RuntimeError("CONFIGQML_TEST_VM_UUID must match the verified guest DMI UUID")
    scratch = Path(os.environ["TMPDIR"]).resolve(strict=True)
    mount = subprocess.run(["findmnt", "-n", "-o", "FSTYPE", "-T", str(scratch)],
                           check=True, text=True, capture_output=True).stdout.strip()
    if str(scratch).startswith("/tmp/") or mount == "tmpfs":
        raise RuntimeError("TMPDIR must be disk-backed project workspace storage")
    env = dict(os.environ)
    for key in ("QML_IMPORT_PATH", "QML2_IMPORT_PATH", "LD_LIBRARY_PATH", "CMAKE_PREFIX_PATH",
                "configqml_DIR"):
        env.pop(key, None)
    env.update(LANG="C.UTF-8", QT_FATAL_WARNINGS="1", QT_QPA_PLATFORM="offscreen",
               QSG_RHI_BACKEND="software")
    if prefix:
        env["LD_LIBRARY_PATH"] = str(Path(prefix) / "lib")
    return env


def run(output, name, command, *, env=None, timeout=1800, expected=0):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    command = [str(arg) for arg in command]
    write_json(output / (name + ".command.json"), command)
    with (output / (name + ".log")).open("w") as log:
        try:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=timeout, env=env)
            rc = result.returncode
        except subprocess.TimeoutExpired:
            rc = 124
    (output / (name + ".rc")).write_text(str(rc) + "\n")
    text = (output / (name + ".log")).read_text()
    if rc != expected:
        raise RuntimeError(f"{name}: rc {rc}, expected {expected}; see {output / (name + '.log')}")
    return text


def require_marker(text, marker):
    if marker not in text:
        raise RuntimeError(f"missing outcome marker: {marker}")


def loader(output, name, executable, prefix, env, *, core_only=False, plugin_only=False):
    text = run(output, name, ["ldd", executable], env=env)
    if "not found" in text:
        raise RuntimeError("unresolved shared library")
    libraries = [line for line in text.splitlines() if "libconfigqml" in line]
    if plugin_only:
        if libraries:
            raise RuntimeError("QtQml-only consumer unexpectedly links ConfigQml")
        return
    if not libraries or any(str(Path(prefix).resolve()) + "/lib/" not in line for line in libraries):
        raise RuntimeError("consumer did not resolve exclusively from the selected install prefix")
    if core_only and any(name in text for name in ("libQt6Qml", "libQt6Quick", "libQt6Gui")):
        raise RuntimeError("Core-only consumer acquired a QML/GUI dependency")


def installed_manifest(prefix, output, env):
    prefix, output = Path(prefix), Path(output)
    libraries = {
        "core": prefix / "lib/libconfigqml_core.so.0.5",
        "qml": prefix / "lib/libconfigqml.so.0.5",
        "plugin": prefix / "lib/qt6/qml/ConfigQml/libconfigqmlplugin.so",
    }
    result = {"headers": {}, "libraries": {}}
    for header in sorted((prefix / "include/configqml").glob("*.h")):
        result["headers"][header.name] = digest(header)
    if not result["headers"]:
        raise RuntimeError("no installed public headers")
    for name, library in libraries.items():
        symbols = run(output, name + "-symbols", ["nm", "-D", "--defined-only", "--format=posix",
                                                   library], env=env)
        dynamic = run(output, name + "-dynamic", ["readelf", "-d", library], env=env)
        if name != "plugin" and f"[{library.name}]" not in dynamic:
            raise RuntimeError("unexpected SONAME")
        exported = sorted({line.split()[0] for line in symbols.splitlines() if line.split()})
        if not exported:
            raise RuntimeError("empty export discovery")
        result["libraries"][name] = {"path": str(library.resolve()), "sha256": digest(library),
                                     "symbols": exported}
    return result
