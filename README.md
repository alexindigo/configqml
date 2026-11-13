# configqml

Layered configuration for Qt 6 and QML applications, with one set of typed
defaults and explicit persistent versus runtime intent.

## Status

**Pre-1.0: the API/ABI may change before 1.0.** A Core-only background owner
provides checked ingress, sequence-fenced read-your-writes, immutable admitted
snapshots, and asynchronous save/flush/close receipts. `ConfigQml 1.0` adds
import-only schemas and real Qt properties; native `Q_PROPERTY` classes can
forward to the same store. Read-only routes expose nonwritable dynamic leaves.
The API is not stable.
Persistence supports preserving JSON files and a deterministic per-store memory
backend. See [JSON files and save policy](docs/json-files.md) for file/shard
declarations, live watching, conditional initialization, cooperative writers and
autoSave (enabled by default, 300ms). Persistence and reload stay worker-owned.
Read-only list/keyed models expose stable schema-selected identity, immutable
nested snapshots and worker-computed incremental updates. See [models](docs/models.md).
Small updates retain delegate identity; large plans use one coherent reset and
provide `indexOfRow(rowId)` for key-based selection recovery.
Detached editor drafts apply one revision-fenced group; scoped handles share that
owner. [Editors and profiles](docs/editors-profiles.md) describes portable raw
exports/imports, and [evolution and secrets](docs/evolution-secrets.md) documents
worker-only raw migrations, per-node admission, redaction and keyring references.
Declared [overlay views and editors](docs/overlay-editors.md) combine a scoped
base with sparse record overrides: live read-only runtime values and separately
captured revision-fenced previews, with worker-owned merge/difference and receipts.

Version 0.4.0 adds raw-preserving record-list container creation and inheritance,
plus opt-in per-source Strict unknown-data closure and open Records. Preserve is
the default. See [compatibility views](docs/compatibility-views.md) and
[source modes](docs/json-files.md#per-source-unknown-data-mode-040).

Version 0.5.0 adds opt-in `StrictPendingRebase`: unfrozen ordinary pending
edits follow accepted foreign files with local-wins leaf intent, retained
evidence and override notices. Retained groups stay frozen. Ordinary publication
failures no longer poison unrelated saves, and exact-byte restoration resolves
strict captures despite a new file inode/mtime. See [strict files](docs/strict-files.md).

Runtime instance writers use [live overlays](docs/overlay-editors.md#runtime-live-writers-050).
Shared edits feed [draft preview views](docs/editors-profiles.md#live-preview-views-050).
Overlay [placementToken](docs/overlay-editors.md#handles-receipts-and-lifetime) tracks observable row moves.
[`copyValue`](docs/worker-qml.md#explicit-detached-copies-050) provides detached mutable copies without losing exact numeric atoms.
BINDABLE state and checked namespace bindings share coherent
[notification preparation](docs/worker.md#input-and-authoritative-settlement).
Bounded [evidence backlogs](docs/strict-files.md) retain acquired bytes across failures.

## Install

Arch Linux package: `qt6-configqml` (AUR; publication follows the first release).
It depends on `qt6-base` and `qt6-declarative`, with CMake/Ninja for building.
Rebuild on Qt minor bumps: the isolated QML metadata adapter uses private QtCore
API and currently requires exact Qt **6.12.0**. See
[distribution](docs/distribution.md) for package layout and compatibility.

From a source archive, inside your build VM:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel 4
sudo cmake --install build
```

Tests default off. `-DCONFIGQML_BUILD_QML=OFF` installs the QtCore-only library.
The packaged module can be imported without application C++ or a custom plugin.

## Native core

Use C++17 and Qt **6.12** on Linux. The production target links only Qt Core;
Qt Test is used by the test executables. With an installed package:

```cmake
find_package(configqml 0.5.0 REQUIRED COMPONENTS Core)
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
[docs/conventions.md](docs/conventions.md) for the complete contract.

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

Run the installed QML example with
`/usr/lib/qt6/bin/qml -a core examples/qml/Settings.qml` on Arch.
The [independent examples](examples/README.md) include a headless C++ consumer
linking only `configqml::core`. The [documentation index](docs/README.md) covers
the shipped native/QML API, persistence, collections, drafts and evolution.

## Development

Builds and tests run inside an isolated test VM with Qt and the build tools
installed there. Use a fresh per-feature or per-release VM, a disk-backed
workspace and TMPDIR, and bounded commands. Record the actual guest, hosting
machine and access route with the verification evidence. If that VM is
unavailable, stop rather than falling back to the physical host.
See [RELEASING.md](RELEASING.md) for the guest-side verification commands.

Enable the tracked hook with `git config core.hooksPath .githooks`. It runs only
clang-format, shellcheck, shfmt and whitespace checks in a resource-capped Podman
container; it does not build or run the project on the laptop. The matching
`.devcontainer/` image is check tooling, not a project test environment.
CI builds and runs CTest on an Ubuntu VM runner using Qt 6.12.0, including the
official SDK's matching ICU runtime. The native owner uses no private Qt API and
does not require Qt Qml (`CONFIGQML_BUILD_QML=OFF`). The dynamic facade's private
metadata adapter records/checks exact Core/CorePrivate/Qml 6.12.0 at configure
time and must be rebuilt with matching Qt.
See [RELEASING.md](RELEASING.md) for sanitizer, fresh-prefix and Arch package gates.

## License

LGPL-3.0-or-later. See [COPYING.LESSER](COPYING.LESSER) and [COPYING](COPYING).
