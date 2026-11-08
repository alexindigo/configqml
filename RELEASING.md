# Releasing configqml — technical gates only

Run these gates before handing a candidate to the release coordinator. Release
publication is a separate owner operation. All builds and tests below run inside
an isolated test VM. Prepare a fresh per-release VM from a clean image, install
the exact Qt version and build tools there, and record the guest name, hosting
machine, disk-backed workspace and access route with the candidate's evidence.
The Arch package gate requires an Arch Linux x86_64 guest. If the selected VM is
unavailable, stop rather than falling back to the physical host.

The commands assume an active session inside that test VM, in its disk-backed
workspace, with the candidate delivered by `git archive` into `source/`.
Set `TMPDIR=$PWD/tmp` and create that directory first. Use a fresh workspace for
each candidate. Source archiving, delivery and checksum collection may run on
the host as read/copy work; compilation and runtime verification stay in the VM.

## Gate 1 — full Release and sanitizer suites

From the guest workspace with the candidate delivered into `source/`:

```sh
timeout 1800 cmake -S source -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
timeout 1800 cmake --build build-release --parallel 4
timeout 1800 ctest --test-dir build-release --output-on-failure --no-tests=error
timeout 1800 cmake -S source -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
timeout 1800 cmake --build build-san --parallel 4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  timeout 1800 ctest --test-dir build-san --output-on-failure --no-tests=error
```

Require nonzero test discovery and 100% passed, no sanitizer findings. The
`installed_prefix` CTest installs to an empty prefix, relocates it, builds/runs
the independent native example, checks resolved libraries and Core-only closure,
lints the QML example and runs it cold with Qt's qml tool. Neither consumer may
resolve a configqml library from the build tree. CI runs the same full suites
on GitHub-hosted Ubuntu VMs with exact Qt 6.12.0 including matching ICU.

The `distribution` CTest label is the shared CI/install-layout matrix. In addition
to the default `installed_prefix` test it includes `install_layout_gnu_dirs`
(lib64 plus nondefault include/data directories), `install_layout_relative_qml`,
`install_layout_absolute_qml` (inside a staged prefix), and
`install_layout_absolute_dirs` (absolute GNU directories, relative QML). Each
builds the production libraries in its own configuration, installs, then lints
and runs both examples against that installation. Fully relative installations
are relocated; absolute destinations retain their configured staged locations.
All layouts also compile the public QML C++ headers/target and check library
resolution. The full Release and sanitizer commands above include this matrix;
use `ctest --test-dir <build> -L distribution -V --no-tests=error` for just that
gate when only installation logic changed.

## Gate 2 — Core-only distribution

```sh
timeout 1800 cmake -S source -B build-core -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCONFIGQML_BUILD_QML=OFF -DCMAKE_INSTALL_PREFIX="$PWD/core-prefix"
timeout 1800 cmake --build build-core --parallel 4
timeout 1800 cmake --install build-core
timeout 1800 cmake -S source/examples/native -B core-consumer -G Ninja -DCMAKE_PREFIX_PATH="$PWD/core-prefix"
timeout 1800 cmake --build core-consumer --parallel 2
timeout 60 core-consumer/configqml-native
```

Require `CONFIGQML_NATIVE_OK 0.4.0`, exit 0, no QtQml imported target or dependency.
BUILD_TESTING defaults off; packaging must not require Qt Test or Quick.

## Gate 3 — Arch package

The host transport creates `qt6-configqml-0.4.0.tar.gz` from the release commit:

```sh
git archive --format=tar.gz --prefix=configqml-0.4.0/ \
  --output=qt6-configqml-0.4.0.tar.gz <release-commit>
```

Archiving/delivery is read/copy work only. It delivers that archive and the
candidate recipe to the guest's `packaging/`, recording the archive SHA256.
The local filename matches the recipe's downloaded filename; makepkg uses it
without a network fetch. In that guest directory:

```sh
timeout 60 makepkg --printsrcinfo > generated.SRCINFO
timeout 60 diff -u .SRCINFO generated.SRCINFO
timeout 1800 makepkg --cleanbuild --noconfirm
timeout 120 sudo pacman -U --noconfirm qt6-configqml-0.4.0-1-x86_64.pkg.tar.zst
timeout 60 namcap PKGBUILD qt6-configqml-0.4.0-1-x86_64.pkg.tar.zst
timeout 60 pacman -Qkk qt6-configqml
env -u QML_IMPORT_PATH -u QML2_IMPORT_PATH -u LD_LIBRARY_PATH QT_FORCE_STDERR_LOGGING=1 \
  timeout 60 /usr/lib/qt6/bin/qml -a core ../source/examples/qml/Settings.qml
env -u CMAKE_PREFIX_PATH -u configqml_DIR -u LD_LIBRARY_PATH \
  timeout 1800 cmake -S ../source/examples/native -B ../package-native -G Ninja
timeout 1800 cmake --build ../package-native --parallel 2
timeout 60 ../package-native/configqml-native
timeout 60 ldd ../package-native/configqml-native
```

Require clean namcap output, 0 altered package files, `CONFIGQML_QML_OK` and
`CONFIGQML_NATIVE_OK 0.4.0`, exit 0, with native libraries from `/usr/lib`.
Check package headers,
CMake exports, libraries, QML metadata/plugin and both license texts. The external
packaging checkout needs the published archive's real checksum before submission.
Rebuild on Qt minor version changes because of the isolated private API.

## Gate 4 — source and release metadata

README/docs/examples describe only the shipped surface. Run the normal signed
commit hook (resource-capped local Podman is approved for these checks). All QML
examples must lint and cold-load. Runtime `configqml::version()`, CMake project
version, packaging version and dated changelog agree. Version/changelog bookkeeping
is the final commit. Retain commands, source identity and outcome evidence outside
the repository, then shut down the assigned VM.
