#!/usr/bin/env python3
"""Check the standalone baseline controls without changing frozen ABI inputs."""

import argparse
from pathlib import Path

from harness import guest_environment, loader, require_marker, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("source", "baseline-source", "baseline-build", "prefix", "frozen", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    env = guest_environment(args.prefix.resolve())
    if args.sanitize:
        env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    source, output = args.source.resolve(), args.output.resolve()
    prefix = args.prefix.resolve()
    flags = (["-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer",
              "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"] if args.sanitize else [])
    for name, directory, extra in (
        ("clients", "compat", []),
        ("red", "red", [f"-DCONFIGQML_BASELINE_SOURCE={args.baseline_source.resolve()}",
                         f"-DCONFIGQML_IMPORT_ROOT={prefix}/lib/qt6/qml"]),
    ):
        build = output / (name + "-build")
        run(output, name + "-configure", ["cmake", "-S", source / directory, "-B", build,
                                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                                          f"-DCMAKE_PREFIX_PATH={prefix}", *flags, *extra], env=env)
        run(output, name + "-build", ["cmake", "--build", build, "--parallel", "2"], env=env)
    for name, marker in (("old-native", "OLD_NATIVE_OK"), ("old-metaobject", "OLD_METAOBJECT_OK")):
        binary = output / "clients-build" / name
        loader(output, name + "-loader", binary, prefix, env, core_only=name == "old-native")
        require_marker(run(output, name, [binary], env=env), marker)
    qml = output / "clients-build/old-qml"
    loader(output, "old-qml-loader", qml, prefix, env, plugin_only=True)
    require_marker(run(output, "old-qml", [qml, prefix / "lib/qt6/qml",
                                           source / "compat/old-client.qml"], env=env), "OLD_QML_OK")
    red = run(output, "behavioral-red", [output / "red-build/configqml-generic-red"],
              env=env, expected=1)
    require_marker(red, "GENERIC_STALE_READ writer=3 reader=4 argv=4")
    require_marker(red, "GENERIC_STALE_BINDING binding=4 released_native=3 released_binding=3")
    for name, fixture in (("red", source / "red/stale-reader.qml"),
                          ("compat", source / "compat/old-client.qml")):
        run(output, name + "-qml-lint", ["/usr/lib/qt6/bin/qmllint", "--missing-property", "error",
                                         "-I", prefix / "lib/qt6/qml", fixture], env=env)
    frozen = run(output, "frozen-inputs", ["python3", source / "capture-baseline.py",
                 "--source", args.baseline_source, "--build", args.baseline_build,
                 "--prefix", prefix, "--output", args.frozen], env=env)
    require_marker(frozen, "BASELINE_CAPTURE_OK retained unchanged baseline")
    print("GENERIC_BASELINE_CONTROLS_OK" + (" fixture-asan-ubsan" if args.sanitize else " release"))


if __name__ == "__main__":
    main()
