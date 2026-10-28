# configqml

Layered configuration for Qt 6 and QML applications.

Applications declare their settings once, read them through ordinary QML
bindings or a native C++ API, and change them with plain property
assignments. Every write goes through the library; merging, validation and
file work run on a background thread, so saving never blocks the UI.

- **Layers** with one documented precedence: defaults → files → environment →
  runtime. Objects merge key by key, lists replace as a whole, a missing value
  inherits, and reset is explicit.
- **One writable user layer.** Saves are sparse, unknown keys are kept, and
  every value knows which layer it came from.
- **Truthful saves:** each save reports *saved*, *nothing to save* or
  *failed* — after the work is actually done.
- **File backend** with atomic writes and change watching that ignores the
  library's own writes; an in-memory backend for tests.
- **List models** for list settings, so views bind without copies.

## Status

Design phase. Nothing usable yet.

## License

LGPL-3.0-or-later. See [COPYING.LESSER](COPYING.LESSER) and [COPYING](COPYING).
