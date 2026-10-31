# Changelog

## [Unreleased]

### Fixed

- JSON object encoding indexes preserved key order instead of repeatedly scanning
  it, avoiding quadratic work for wide objects while retaining deterministic order.
- Migration and source-projection bridges recover exact numeric/URL wrappers with
  an identity-checked index rather than a full scan for each returned value.

### Security

- Profile imports refuse non-regular files and enforce a configurable 16 MiB byte
  limit before and during reads, preventing FIFO blocking and unbounded buffering.
- Profile export admits whitelisted raw values through their descriptors, refusing
  malformed scalar/collection content that could carry undeclared private data.
- Profile publication checks and writes the same resolved target, including dangling
  symlinks, preventing export aliases from overwriting configured sources.

## [0.2.0] — 2026-10-29

### Added

- Nonpersisting worker-owned Source projections with exact numeric atoms, ordered
  effects and preserved original raw data. Conditional whole-source fallback,
  startup quarantine and accepted-disk source events expose explicit presence and
  provenance without moving file or merge ownership to the GUI.
- Declared record-list normalization and generation-fenced field/membership edits
  preserve hidden raw rows and avoid persisting injected housekeeping defaults.
- Retained groups reserve selected intent across failures, with immutable captures,
  explicit Retry, causal dependencies and per-attempt publication evidence.
- Strict whole-source preimage conflicts and owner-only observation artifacts,
  plus delete-empty publication that preserves configured symlinks and watches.
- Two-owner overlay views and detached editors, with declared merge/difference
  policies, selected asynchronous leases, frozen revision/intent fences and
  truthful save/refresh receipts. Conflicts retain the editor's dirty preview.
- Generic schema-compiled full-string `stringPattern` constraints, shared by
  native/QML and every canonical default, raw, nested and editor producer.
- Separate `sourceWarning` delivery for nonfatal authored projection diagnostics;
  `sourceError` remains failures-only.

### Changed

- `ConfigDescriptor` is no longer an aggregate, to own compiled constraint metadata
  safely. Default construction, factories and `ConfigDescriptor{Type::String}`
  remain supported through the explicit type constructor.

### Fixed

- Unchanged source preflight preserves accepted pending-intent provenance instead
  of inventing a foreign displayed generation.
- Descriptor assignment captures nested source metadata before releasing its
  owning subtree, including compiled String constraints.

## [0.1.0] — 2026-10-28

### Added

- Standalone C++17/QtCore-only engine and asynchronous store, with typed schemas,
  strict defaults, optional/nullable values, exact 64-bit integers and finite reals.
- Fixed defaults → ordered files → environment → runtime precedence, preserving
  raw unknown/invalid data, explicit default-valued choices and separate provenance.
  Named writable routes, reset and injected typed environment mapping keep
  persistent intent separate from transient overrides.
- `ConfigQml 1.0` import-only schemas and real writable meta-properties, plus
  native property forwarding. Checked unmasked writes read back immediately;
  sequence-fenced worker publications preserve newer intent. Masked writes retain
  effective getters and report the masking layer. Ordinary QML copies are detached.
- Ordered asynchronous save/flush/close with saved, nothing_to_save or failed
  receipts, prefix/revision coverage, retained failure evidence and consumer-safe
  notification/callback lifetime handling. Autosave defaults to 300ms coalescing;
  process shutdown provides a shared bounded settlement drain.
- Preserving JSON files, explicit sharded sources, atomic per-file publication
  and readback, symlink-preserving target resolution, cooperative multi-process
  conflict detection, conditional initialization and selectable live watches.
- Read-only native/QML list and keyed projections with immutable nested nodes,
  stable declared row identity, worker-planned incremental changes, and coherent
  resets for large plans with key-based selection recovery.
- Revision-fenced detached editor drafts, shared-owner scoped handles, portable
  whitelisted raw profiles, worker-only versioned migrations and source admission
  policies. Sensitive/forbidden metadata, value-free diagnostics and opaque secret
  references keep credential providers application-owned.
- Installed `configqml::core` and `configqml::configqml` CMake targets, public
  headers, generated QML plugin/type metadata, relocatable installed-prefix
  consumers and an Arch `qt6-configqml` packaging candidate.

### Quality

- Full native/QML CTest coverage, QtCore-only dependency checks, QML lint and cold
  consumers, Release and ASan/UBSan CI lanes, and resource-capped Podman hooks.
- Private QtCore metadata construction isolated to one translation unit with
  exact Core/CorePrivate/Qml 6.12.0 build identity checks. Rebuild on Qt minor bumps.
- Runtime and library versions derive from CMake's project version. This initial
  pre-1.0 API/ABI may change before 1.0.

### Docs

- Native/QML quick starts, executable standalone examples, complete shipped
  contracts and technical release gates.

[Unreleased]: https://github.com/alexindigo/configqml/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/alexindigo/configqml/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/alexindigo/configqml/releases/tag/v0.1.0
