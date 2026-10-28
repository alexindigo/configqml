# configqml

Layered configuration for Qt 6 and QML applications, with one set of typed
defaults and explicit persistent versus runtime intent.

## Status

**M3: experimental JSON persistence, ordered store and QML facade.** A Core-only background owner
provides checked ingress, sequence-fenced read-your-writes, immutable admitted
snapshots, and asynchronous save/flush/close receipts. `ConfigQml 1.0` adds
import-only schemas and real Qt properties; native `Q_PROPERTY` classes can
forward to the same store. Read-only routes expose nonwritable dynamic leaves.
The API is not stable.
Persistence supports preserving JSON files and a deterministic per-store memory
backend. See [JSON files and save policy](docs/json-files.md) for file/shard
declarations, live watching, conditional initialization, cooperative writers and
autoSave (enabled by default, 300ms). Persistence and reload stay worker-owned.

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

## QML

Link `configqml::configqml` for native QML integration, or import the generated
`ConfigQml` module without application schema C++:

```qml
import QtQml
import ConfigQml 1.0

ConfigStore {
    id: store
    schema: ConfigSchema {
        ConfigGroup {
            key: "bar"
            ConfigValue {
                key: "outputs"; valueType: ConfigValue.StringList
                defaultValue: []
            }
        }
    }
    sources: [MemorySource { layerId: "user"; sourceId: "preferences"; writable: true }]
    function addOutput(name) {
        values.bar.outputs.push(name)
        save(function(result) { console.log(result.status) })
    }
}
```

Gate dependent initialization on `ready`. Unmasked property assignments read
back immediately, including consecutive list mutations; authoritative admission
and publication remain asynchronous. Masked assignments retain the higher-layer
getter and report the mask. Detached copies change the setting only when assigned
back. Integer is a safe JS number; Int64/UInt64 are decimal strings throughout
nested collections. See [docs/worker-qml.md](docs/worker-qml.md) for the full QML
surface and shared notification/reentry lifetime contract.

## Development

All builds/tests for this project run in **agora VM 806 `dev-niri-configqml`**,
accessed via `/home/user/.local/bin/agora-niri`, in the disk-backed workspace
`/home/tester/Projects/cqm3`. If unavailable, stop rather than using a host.
After delivering source to its `source/` subdirectory, from the laptop:

```sh
timeout 240s /home/user/.local/bin/agora-niri ssh 806 -- 'timeout 210s env TMPDIR=/home/tester/Projects/cqm3/tmp LANG=C.UTF-8 QT_FATAL_WARNINGS=1 bash -c "cmake -S /home/tester/Projects/cqm3/source -B /home/tester/Projects/cqm3/build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build /home/tester/Projects/cqm3/build --parallel 4 && ctest --test-dir /home/tester/Projects/cqm3/build --output-on-failure --no-tests=error"'
```

Enable the tracked hook with `git config core.hooksPath .githooks`. It runs only
clang-format, shellcheck, shfmt and whitespace checks in a resource-capped Podman
container; it does not build or run the project on the laptop. The matching
`.devcontainer/` image is check tooling, not a project test environment.
CI builds and runs CTest on an Ubuntu VM runner using Qt 6.12.0, including the
official SDK's matching ICU runtime. The native owner uses no private Qt API and
does not require Qt Qml (`CONFIGQML_BUILD_QML=OFF`). The dynamic facade's private
metadata adapter records/checks exact Core/CorePrivate/Qml 6.12.0 at configure
time and must be rebuilt with matching Qt.

## License

LGPL-3.0-or-later. See [COPYING.LESSER](COPYING.LESSER) and [COPYING](COPYING).
