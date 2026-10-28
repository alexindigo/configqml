# Changelog

## [Unreleased]

### M2 — QML integration

ConfigQml 1.0 adds import-only typed schemas, dynamic real meta-properties and
native property forwarding. Shared consumer guards stop grouped notifications,
rejections and callbacks after emitter destruction. Read-only dynamic leaves
are nonwritable and explicit writes fail before local exposure. QML safe
integers and exact decimal-string 64-bit values share native admission; the
private metadata adapter is isolated and exact-Qt-version checked.

### M2 — native ordered owner

Added a per-store ordered background owner, immediate checked unmasked reads,
sequence/generation-fenced authoritative publications, ordered asynchronous
save/flush/close receipts and deterministic memory fault/lifetime coverage.
Consumer emissions and callbacks share a lifetime guard, pending terminal
rejections publish covering authoritative rollback, and immutable write-route
capability rejects read-only ingress before local exposure. SafeInteger adds the
JavaScript-exact domain without changing native exact 64-bit descriptors.
Persistence remains memory-only.

### Added

- QtCore-only C++17 foundation targeting Qt 6.12, with native dependency checks,
  VM-run CTest coverage and Podman formatting/lint hooks.
- Owned schema/value descriptors, strict defaults and typed list/record/map
  validation, optional/nullable values and value-free diagnostics.
- Preserved raw layers, fixed precedence, explicit admission policies, presence,
  distinct raw/admitted provenance and named user/shard versus runtime writes.
- Opaque scoped handles, reversible scope keys and injected typed environment mappings.
- Backend/codec capability contracts and a deterministic revision-aware memory
  backend with delayed and faulted operation controls.
- Native API and layer conventions documentation.

### Fixed

- Retain exact structured numeric input through environment, raw-layer admission
  and codec value boundaries; reject fractions/overflow instead of accepting rounded integers.
- Reject descriptor constraints that their namespace/type cannot enforce, and
  enforce choices on admitted collection values and explicit nullable values.
- Include the matching ICU runtime in the Qt 6.12 CI SDK selection.
