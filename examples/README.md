# Installed configqml examples

These independent consumers use the public installed library. Neither requires
Atmosphera, Quickshell, a service or an application-specific QML plugin.

| # | Example | Observed endpoint |
|---|---|---|
| 1 | `qml/Settings.qml` | Two property-origin appends compose immediately; close reports saved and the consumer reads both outputs, then exits. |
| 2 | `native/` | A headless QtCore-only process writes a setting, closes the asynchronous store and checks the admitted snapshot. |

With a system installation, run the QML example using Qt's `qml` tool (`qml6`
on some distributions; Arch: `/usr/lib/qt6/bin/qml`):

```sh
qml -a core examples/qml/Settings.qml
cmake -S examples/native -B native -G Ninja
cmake --build native
native/configqml-native
```

For a custom install, set `QML_IMPORT_PATH` to the resolved
`CONFIGQML_QML_INSTALL_DIR` (relative to the prefix unless explicitly absolute).
Set `-DCMAKE_PREFIX_PATH=<prefix>` for the native configure, or point
`-Dconfigqml_DIR` directly to the resolved `${CMAKE_INSTALL_LIBDIR}/cmake/configqml`
directory for a nonstandard library location. The default QML directory retains
Qt's QML-to-library relationship under `CMAKE_INSTALL_LIBDIR`.
Both examples use a per-store memory source, so they do not create user files.
To persist settings, replace MemorySource with an explicit writable JsonFileSource;
see [JSON declarations](../docs/json-files.md). Expected output markers are
`CONFIGQML_QML_OK` and `CONFIGQML_NATIVE_OK`, respectively, with exit status zero.

Project verification executes these commands inside the assigned development VM;
see [RELEASING.md](../RELEASING.md) for the complete gates.
