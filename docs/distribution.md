# Installation and compatibility

configqml 0.2 targets C++17 and Qt 6.12 on Linux. The initial verified platform
is x86_64 Arch Linux. Before 1.0 the API/ABI may change; rebuild consumers when
upgrading. CMake package version matching uses SameMinorVersion, and the shared
libraries have SONAME major 0. `ConfigQml 1.0` is the QML import revision, not a
claim that the library has reached stable version 1.0.

## Installed targets

```cmake
find_package(configqml 0.2 REQUIRED COMPONENTS Core)
target_link_libraries(headless PRIVATE configqml::core)
# For native QML integration instead:
find_package(configqml 0.2 REQUIRED COMPONENTS Qml)
target_link_libraries(declarative PRIVATE configqml::configqml)
```

Core discovery requires QtCore only; it neither finds QtQml nor imports the QML
target. Qml (or default full-package discovery) requires the exact QtQml version
of the private metadata build. `CONFIGQML_BUILD_QML=OFF` builds/installs only Core.
With `COMPONENTS Core OPTIONAL_COMPONENTS Qml`, unavailable matching QtQml leaves
`configqml_Qml_FOUND` false while Core remains usable.
No private headers or private Qt targets appear in the public API/export closure.
Public headers install under `${CMAKE_INSTALL_INCLUDEDIR}/configqml`; both exported targets supply that
include directory, so `#include <configstore.h>` works without manual paths.

Both libraries install under GNUInstallDirs' library directory, and CMake package
files under `${CMAKE_INSTALL_LIBDIR}/cmake/configqml`. The generated QML plugin,
qmldir and qmltypes install under `${CONFIGQML_QML_INSTALL_DIR}/ConfigQml`.
The default QML directory retains Qt's QML-to-library relationship beneath the
configured library directory. Override it with an ordinary command-line value
such as `-DCONFIGQML_QML_INSTALL_DIR=imports/qml`.

Every destination follows GNUInstallDirs semantics: relative values are relative
to `CMAKE_INSTALL_PREFIX`; absolute values remain pinned to that location.
`CMAKE_INSTALL_LIBDIR`, `CMAKE_INSTALL_INCLUDEDIR`, `CMAKE_INSTALL_DATADIR` and
`CONFIGQML_QML_INSTALL_DIR` all accept either form. Header installation, both target
include interfaces, CMake config/export destinations and test lookup derive from
the same variables. Absolute destinations are not rebased by `cmake --install
--prefix`; configure the intended final prefix before calculating mixed-layout
plugin RPATHs. Relative origin-based library/plugin RPATHs keep a fully relative
installation relocatable; explicit absolute destinations retain their identity.
Build-tree RPATHs are removed on install. Packagers using the system loader may
set `CMAKE_SKIP_RPATH=ON`. License texts install under
`${CMAKE_INSTALL_DATADIR}/licenses/configqml`.

CTest and CI share the installed-consumer layout matrix: default, lib64 with
nondefault include/data directories, relative QML override, absolute QML under
a staged prefix, and absolute library/include/data directories with relative QML.
Both functional examples, public C++ QML integration, lint and loader-path checks
run for every layout. Absolute-layout test destinations stay in the build's
disposable `installed-consumer` workspace rather than system directories.
The harness supplies only the staged library directory and discovered Qt's
library, QML and plugin paths, discarding inherited runtime paths. This also
supports non-system Qt SDKs. Installed RPATHs are checked independently, and
configqml's libraries must still resolve exclusively from the installed prefix,
even when another configqml is installed alongside system Qt.

## Qt rebuild contract

Only `confignamespace.cpp` uses QtCore private API (QMetaObjectBuilder). CMake
requires exact Core/CorePrivate/Qml **6.12.0** and installs the recorded build
identity in `${CMAKE_INSTALL_DATADIR}/configqml/configqml-qt-build.txt`. Rebuild on Qt minor bumps;
for a new Qt version first update and verify the exact adapter gate, rather than
pretending a binary built for an old private ABI is compatible. A distribution's
Qt package dependencies are broader than the library's CMake linkage: Arch's
qt6-base includes graphics and system D-Bus, and QtQml itself links QtNetwork.
Core-only linkage does not mean a graphics-free Arch package closure.

## Packaging candidate

`packaging/aur/qt6-configqml` contains a tagged-tarball recipe and `.SRCINFO`.
It is a candidate for separate release publication. The in-tree recipe cannot
contain the checksum of the archive containing itself: its checksum placeholder
must be replaced with the published release archive hash in the external AUR
checkout before publication. Local verification uses a separately hashed archive
of the release commit, without contacting the unpublished tag.
The recipe pins qt6-base and qt6-declarative to the adapter's exact Qt version,
so an incompatible Qt upgrade cannot silently retain the old binary plugin.
