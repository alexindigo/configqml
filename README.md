# configqml

Layered configuration for Qt 6 and QML applications, with one set of typed
defaults and explicit persistent versus runtime intent.

## Status

**M1: experimental QtCore-only foundation.** The native synchronous engine,
typed schema, presence/provenance, preserving raw layers, scoped handles,
injected environment adapter and deterministic memory backend are implemented.
The API is not stable. The background store/worker and QML facade follow in M2;
file persistence follows in M3. No asynchronous store, save API or QML module is
provided by this milestone.

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

## Development

All builds/tests for this project run in **agora VM 806 `dev-niri-configqml`**,
accessed via `/home/user/.local/bin/agora-niri`, in the disk-backed workspace
`/home/tester/Projects/cqm1`. If unavailable, stop rather than using a host.
After delivering source to its `source/` subdirectory, from the laptop:

```sh
timeout 180s /home/user/.local/bin/agora-niri ssh 806 -- 'timeout 150s env TMPDIR=/home/tester/Projects/cqm1/tmp bash -lc "cmake -S /home/tester/Projects/cqm1/source -B /home/tester/Projects/cqm1/build -G Ninja && cmake --build /home/tester/Projects/cqm1/build --parallel 4 && ctest --test-dir /home/tester/Projects/cqm1/build --output-on-failure --no-tests=error"'
```

Enable the tracked hook with `git config core.hooksPath .githooks`. It runs only
clang-format, shellcheck, shfmt and whitespace checks in a resource-capped Podman
container; it does not build or run the project on the laptop. The matching
`.devcontainer/` image is check tooling, not a project test environment.
CI builds and runs CTest on an Ubuntu VM runner using Qt 6.12.0, including the
official SDK's matching ICU runtime. Private headers
are available there for the later isolated QML adapter; M1 uses no private Qt API.

## License

LGPL-3.0-or-later. See [COPYING.LESSER](COPYING.LESSER) and [COPYING](COPYING).
