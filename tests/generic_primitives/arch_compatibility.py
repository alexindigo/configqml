"""Matched Arch hardening/LTO ABI lane, including clients frozen before candidate build."""

import json
from pathlib import Path
import shlex
import shutil

from harness import digest, guest_environment, installed_manifest, loader, require_marker, run, write_json


def arch_flags(output, env):
    # Use the installed makepkg configuration, not guessed packaging flags.
    # These files belong to the assigned test guest, not the physical host.
    text = run(output, "makepkg-flags", ["bash", "-c", r'''
set -eu
source /etc/makepkg.conf
for config in /etc/makepkg.conf.d/*.conf; do
    if test -f "$config"; then source "$config"; fi
done
python3 - "$CFLAGS" "$CXXFLAGS" "$LDFLAGS" "$LTOFLAGS" "${OPTIONS[@]}" <<'PY'
import json, sys
print(json.dumps(dict(zip(('c', 'cxx', 'link', 'lto'), sys.argv[1:5])) |
                 {'options': sys.argv[5:]}))
PY
'''], env=env)
    flags = json.loads(text)
    if "lto" not in flags["options"] or "!lto" in flags["options"] or "-flto" not in flags["lto"]:
        raise RuntimeError("Arch matched lane requires the recipe's enabled LTO")
    for required in ("-fstack-clash-protection", "-fcf-protection"):
        if required not in flags["cxx"]:
            raise RuntimeError("Arch hardening flag missing: " + required)
    for required in ("relro", "now"):
        if required not in flags["link"]:
            raise RuntimeError("Arch linker hardening missing: " + required)
    write_json(output / "flags.json", flags)
    return flags


def recipe_build(source, output, flags, env):
    build, prefix = output / "build", output / "prefix"
    # Same generator, Release mode, RPATH and library-only build as the recipe.
    # A private prefix avoids replacing either installed system or frozen DSOs.
    run(output, "configure", ["cmake", "-S", source, "-B", build, "-G", "Unix Makefiles",
        "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF", "-DCMAKE_SKIP_RPATH=ON",
        f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib",
        "-DCONFIGQML_QML_INSTALL_DIR=lib/qt6/qml",
        f"-DCMAKE_C_FLAGS={flags['c']} {flags['lto']}",
        f"-DCMAKE_CXX_FLAGS={flags['cxx']} {flags['lto']}",
        f"-DCMAKE_SHARED_LINKER_FLAGS={flags['link']} {flags['lto']}",
        f"-DCMAKE_MODULE_LINKER_FLAGS={flags['link']} {flags['lto']}"], env=env)
    run(output, "build", ["cmake", "--build", build, "--parallel", "2"], env=env)
    run(output, "install", ["cmake", "--install", build], env=env)
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith(("CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS:", "CMAKE_CXX_FLAGS_RELEASE:",
                            "CMAKE_SHARED_LINKER_FLAGS:", "CMAKE_MODULE_LINKER_FLAGS:")):
            name, value = line.split("=", 1)
            cache[name] = value
    if len(cache) != 5:
        raise RuntimeError("incomplete matched build flag discovery")
    write_json(output / "effective-flags.json", cache)
    # Evidence the flags reached actual compiler/link commands, not only cache.
    compile_flags = (build / "CMakeFiles/configqml_core.dir/flags.make").read_text()
    link_flags = (build / "CMakeFiles/configqml_core.dir/link.txt").read_text()
    for flag in shlex.split(flags["cxx"] + " " + flags["lto"]):
        if flag not in compile_flags:
            raise RuntimeError("compile command omitted Arch flag: " + flag)
    for flag in shlex.split(flags["link"] + " " + flags["lto"]):
        if flag not in link_flags:
            raise RuntimeError("link command omitted Arch flag: " + flag)
    (output / "compiler-flags.txt").write_text(compile_flags)
    (output / "link-command.txt").write_text(link_flags)
    return prefix, cache


def freeze_arch_clients(baseline, source, prefix, output, flags):
    original = json.loads((baseline / "baseline.json").read_text())
    env = guest_environment(prefix)
    output.mkdir(parents=True, exist_ok=False)
    for name in ("public-layouts.cpp", "old-client.qml"):
        shutil.copy2(baseline / name, output / name)
    # The first baseline capture predates source-copy retention. These tracked
    # public-API fixtures also build against 0.5.3; original frozen executables
    # are independently run unchanged below, never replaced by these new ones.
    clients = baseline / "client-sources"
    if not clients.is_dir():
        clients = source / "tests/generic_primitives/compat"
    retained = output / "client-sources"
    shutil.copytree(clients, retained)
    clients = retained
    build = output / "clients-build"
    run(output, "configure", ["cmake", "-S", clients, "-B", build, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={prefix}",
        f"-DCONFIGQML_LAYOUT_SOURCE={output / 'public-layouts.cpp'}",
        f"-DCMAKE_CXX_FLAGS={flags['cxx']} {flags['lto']}",
        f"-DCMAKE_EXE_LINKER_FLAGS={flags['link']} {flags['lto']}"], env=env)
    run(output, "build", ["cmake", "--build", build, "--parallel", "2"], env=env)
    binaries = {name: str(Path("clients-build") / name) for name in original["binaries"]}
    for name, marker in (("old-native", "OLD_NATIVE_OK"), ("old-metaobject", "OLD_METAOBJECT_OK")):
        require_marker(run(output, name, [output / binaries[name]], env=env), marker)
    require_marker(run(output, "old-qml", [output / binaries["old-qml"],
        prefix / "lib/qt6/qml", output / "old-client.qml"], env=env), "OLD_QML_OK")
    abi = json.loads(run(output, "public-layouts", [build / "public-layouts"], env=env))
    write_json(output / "public-abi.json", abi)
    installation = installed_manifest(prefix, output, env)
    frozen = [output / path for path in binaries.values()] + [output / "public-layouts.cpp",
        output / "public-abi.json", output / "old-client.qml"] + sorted(retained.iterdir())
    write_json(output / "baseline.json", {
        "schema": 1, "source": original["source"], "binaries": binaries,
        "installation": installation,
        "frozen_files": {str(path.relative_to(output)): digest(path) for path in frozen}})
    return output


def matched_arch_lane(baseline, source, output, check_lane, consumers, roots):
    output.mkdir(parents=True, exist_ok=False)
    env = guest_environment()
    original = json.loads((baseline / "baseline.json").read_text())
    flags = arch_flags(output, env)
    run(output, "compiler", ["c++", "--version"], env=env)
    baseline_source = Path(original["source"])
    if "VERSION 0.5.3 " not in (baseline_source / "CMakeLists.txt").read_text():
        raise RuntimeError("matched lane needs the retained v0.5.3 sources")
    before_prefix, before_flags = recipe_build(baseline_source, output / "baseline", flags, env)
    frozen = freeze_arch_clients(baseline, source, before_prefix, output / "old-clients", flags)
    # Freeze and run old clients before even building candidate DSOs.
    after_prefix, after_flags = recipe_build(source, output / "candidate", flags, env)
    if before_flags != after_flags:
        raise RuntimeError("Arch baseline/candidate compiler or flags differ")
    result = check_lane(frozen, source, after_prefix, output / "compatibility", consumers, roots)
    # Also run ORIGINAL plain-built frozen binaries against Arch DSOs. Do not
    # compare plain baseline exports with LTO exports: only the matched pair
    # above provides the packaging-symbol comparison.
    client_env = guest_environment(after_prefix)
    for name, marker in (("old-native", "OLD_NATIVE_OK"), ("old-metaobject", "OLD_METAOBJECT_OK")):
        executable = baseline / original["binaries"][name]
        loader(output, name + "-original-loader", executable, after_prefix, client_env,
               core_only=name == "old-native")
        require_marker(run(output, name + "-original", [executable], env=client_env), marker)
    executable = baseline / original["binaries"]["old-qml"]
    loader(output, "old-qml-original-linkage", executable, after_prefix, client_env, plugin_only=True)
    text = run(output, "old-qml-original", [executable, after_prefix / "lib/qt6/qml",
        baseline / "old-client.qml"], env=client_env)
    require_marker(text, "OLD_QML_OK")
    origins = [line for line in text.splitlines() if line.startswith("OLD_QML_LOADER ")]
    if not any("libconfigqmlplugin.so" in line for line in origins) or any(
            str(after_prefix) + "/lib/" not in line for line in origins):
        raise RuntimeError("original client loaded outside matched Arch prefix")
    for path, expected in original["frozen_files"].items():
        if digest(baseline / path) != expected:
            raise RuntimeError("original frozen input changed: " + path)
    result["flags"] = flags
    result["effective_flags"] = before_flags
    return result
