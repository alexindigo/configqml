#!/usr/bin/env python3
"""Freeze real v0.5.3 consumers before introducing candidate shared libraries."""

import argparse
import json
from pathlib import Path
import shutil

from harness import digest, guest_environment, installed_manifest, loader, require_marker, run, write_json
from layout_contract import generate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("source", "build", "prefix", "output"):
        parser.add_argument("--" + option, required=True, type=Path)
    args = parser.parse_args()
    source, build, prefix, output = (getattr(args, key).resolve() for key in
                                     ("source", "build", "prefix", "output"))
    env = guest_environment(prefix)
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = output / "baseline.json"
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
        for path, expected in manifest["frozen_files"].items():
            if digest(output / path) != expected:
                raise RuntimeError("frozen baseline changed: " + path)
        print("BASELINE_CAPTURE_OK retained unchanged baseline")
        return
    if "VERSION 0.5.3 " not in (source / "CMakeLists.txt").read_text():
        raise RuntimeError("baseline source must be released v0.5.3")
    tests = run(output, "baseline-discovery", ["ctest", "--test-dir", build, "--show-only=json-v1"], env=env)
    discovered = json.loads(tests)["tests"]
    if len(discovered) != 97:
        raise RuntimeError(f"baseline discovery expected 97, found {len(discovered)}")
    # Copy only installed public headers, not a source/private-layout snapshot.
    headers = output / "headers"
    headers.mkdir(exist_ok=True)
    for header in (prefix / "include/configqml").glob("*.h"):
        shutil.copy2(header, headers / header.name)
    frozen_headers = sorted(headers.glob("*.h"))
    layout_source = output / "public-layouts.cpp"
    layout_source.write_text(generate(frozen_headers))
    clients = Path(__file__).resolve().parent / "compat"
    client_build = output / "clients-build"
    run(output, "clients-configure", ["cmake", "-S", clients, "-B", client_build, "-G", "Ninja",
                                      "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={prefix}",
                                      f"-DCONFIGQML_LAYOUT_SOURCE={layout_source}"], env=env)
    run(output, "clients-build", ["cmake", "--build", client_build, "--parallel", "2"], env=env)
    binaries = {}
    for name, marker in (("old-native", "OLD_NATIVE_OK"), ("old-metaobject", "OLD_METAOBJECT_OK")):
        executable = client_build / name
        loader(output, name + "-loader", executable, prefix, env, core_only=name == "old-native")
        text = run(output, name, [executable], env=env)
        require_marker(text, marker)
        binaries[name] = str(executable.relative_to(output))
    qml = client_build / "old-qml"
    loader(output, "old-qml-linkage", qml, prefix, env, plugin_only=True)
    fixture = output / "old-client.qml"
    shutil.copy2(clients / "old-client.qml", fixture)
    text = run(output, "old-qml", [qml, prefix / "lib/qt6/qml", fixture], env=env)
    require_marker(text, "OLD_QML_OK")
    loaded = [line for line in text.splitlines() if line.startswith("OLD_QML_LOADER ")]
    if not loaded or not any("libconfigqmlplugin.so" in line for line in loaded) or any(
            str(prefix) + "/lib/" not in line for line in loaded):
        raise RuntimeError("pure QML consumer did not load exclusively from baseline prefix")
    binaries["old-qml"] = str(qml.relative_to(output))
    layouts = run(output, "public-layouts", [client_build / "public-layouts"], env=env)
    abi = json.loads(layouts)
    if not abi["layouts"] or not abi["metadata"]:
        raise RuntimeError("empty ABI probe output")
    write_json(output / "public-abi.json", abi)
    installation = installed_manifest(prefix, output, env)
    cache = (build / "CMakeCache.txt").read_text()
    (output / "baseline-cmake-cache.txt").write_text(cache)
    client_sources = output / "client-sources"
    client_sources.mkdir(exist_ok=True)
    for original in clients.iterdir():
        if original.is_file():
            shutil.copy2(original, client_sources / original.name)
    frozen = [layout_source, fixture, output / "public-abi.json", output / "baseline-cmake-cache.txt"]
    frozen += sorted(client_sources.iterdir())
    frozen += frozen_headers + [output / path for path in binaries.values()]
    manifest = {"schema": 1, "version": "0.5.3", "source": str(source), "build": str(build),
                "prefix": str(prefix), "test_count": len(discovered), "binaries": binaries,
                "installation": installation,
                "frozen_files": {str(path.relative_to(output)): digest(path) for path in frozen}}
    write_json(manifest_path, manifest)
    print(f"BASELINE_CAPTURE_OK {len(binaries)} frozen clients, "
          f"{len(abi['layouts'])} layouts, {len(abi['metadata'])} metaobjects")


if __name__ == "__main__":
    main()
