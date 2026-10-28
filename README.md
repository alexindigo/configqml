# configqml

Layered configuration for Qt 6 and QML applications, with one set of typed
defaults and explicit persistent versus runtime intent.

## Status

**M2 native foundation: experimental ordered store.** A Core-only background owner
provides checked ingress, sequence-fenced read-your-writes, immutable admitted
snapshots, and asynchronous save/flush/close receipts. Native `Q_PROPERTY`
classes can forward to the same store. The API is not stable.
Persistence uses a deterministic per-store memory backend; files follow in M3.

## Native core

Use C++17 and Qt **6.12** on Linux. The production target links only Qt Core;
Qt Test is used by the test executables. With this checkout as a CMake subdirectory:

```cmake
add_subdirectory(configqml)
target_link_libraries(myapp PRIVATE configqml::core)
```

```cpp
#include "configlayers.h"
using namespace configqml;
using D = ConfigDescriptor;

ConfigSchema schema(D::group({
    {"position", D::leaf(D::Type::String, QStringLiteral("bottom"))}
}));
ConfigLayer user{"user", ConfigLayerKind::File,
    ConfigAdmissionPolicy::FieldFallback, {{"user-settings", {}, {}}}, true};
ConfigLayers engine(schema, {user});
auto result = engine.setUser({"position"}, QStringLiteral("top"));
// result.accepted checks schema admission, not persistence.
auto position = engine.value({"position"});
auto provenance = engine.info({"position"});
```

Paths are segment arrays, not dotted strings. The conventions are
**defaults → explicitly ordered files → environment → runtime**. Groups deep-merge,
lists/records replace, missing inherits, and reset explicitly removes an override.
Equal-to-default assignments remain explicit choices. Environment/runtime masks
are reported without rewriting the stored fallback. See
[docs/conventions.md](docs/conventions.md) for the complete M1 contract.

## Native asynchronous store

Construct `ConfigStore` with owned `ConfigStoreOptions` on its object thread.
Once Ready, `setUser(path, value)` provides checked local read-your-writes;
`save(context, callback)` publishes its captured input prefix asynchronously.
`flush` saves and settles that prefix; `close` also seals ingress and releases
the worker. Read-only paths reject persistent writes before local exposure.
Worker failure settles each pending input back to authoritative state before
failure delivery. See [docs/worker.md](docs/worker.md) for the native contract.

## Development

All builds/tests for this project run in **agora VM 806 `dev-niri-configqml`**,
accessed via `/home/user/.local/bin/agora-niri`, in the disk-backed workspace
`/home/tester/Projects/cqm2/revision-native`. If unavailable, stop rather than using a host.
After delivering source to its `source/` subdirectory, from the laptop:

```sh
timeout 210s /home/user/.local/bin/agora-niri ssh 806 -- 'timeout 180s env TMPDIR=/home/tester/Projects/cqm2/revision-native/tmp QT_FATAL_WARNINGS=1 bash -lc "cmake -S /home/tester/Projects/cqm2/revision-native/source -B /home/tester/Projects/cqm2/revision-native/build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build /home/tester/Projects/cqm2/revision-native/build --parallel 4 && ctest --test-dir /home/tester/Projects/cqm2/revision-native/build --output-on-failure --no-tests=error"'
```

Enable the tracked hook with `git config core.hooksPath .githooks`. It runs only
clang-format, shellcheck, shfmt and whitespace checks in a resource-capped Podman
container; it does not build or run the project on the laptop. The matching
`.devcontainer/` image is check tooling, not a project test environment.
CI builds and runs CTest on an Ubuntu VM runner using Qt 6.12.0, including the
official SDK's matching ICU runtime. The native owner uses no private Qt API and
does not require Qt Qml. The QML integration is a separate review unit.

## License

LGPL-3.0-or-later. See [COPYING.LESSER](COPYING.LESSER) and [COPYING](COPYING).
