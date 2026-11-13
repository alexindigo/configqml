# Changelog

## [Unreleased]

## [0.5.0] - 2026-11-12

### Added

- Opt-in `StrictPendingRebase` publication policy for unfrozen ordinary
  pending work. Locked preflight accepts foreign observations before applying
  local-wins leaf edits, preserving foreign siblings and deletions. Retained
  groups and failed attempts stay frozen; structural conflicts still refuse.
  No-effect refusals pin the original capture for content-restoration resolution;
  failures before a successful locked read do not freeze work. A frozen source
  holds newer edits strictly too.
- Route receipt fields `rebasedOnto` and `overriddenPaths`, with value-free
  `foreign-value-overridden` source warnings and retained foreign-byte evidence.
- Writable overlay `mode: "live"` for runtime instance settings. Checked setters
  read back immediately, queue owning-leaf admission without editor busy fences,
  keep sparse overrides and refresh on base/owner changes. `save()`/`flush()` and
  autosave use normal owner persistence, receipts and evidence.
- Read-only live `draft.preview` and `draft.previewScope(path)` overlay bases.
  Edited leaves show draft values; unedited leaves follow current effective
  store values without changing frozen Apply/conflict fences. Preview scopes
  close on successful Apply/discard or draft destruction.
- Overlay `placementToken` fences observable raw-row mapping changes without
  persistent row IDs. Member edits and indistinguishable same-ID replacements
  keep the token; observable moves/reorders and identity changes update it.
- `copyValue` makes detached JS-mutable deep copies with exact native numeric
  atoms and own prototype-like keys preserved.
- BINDABLE store and overlay
  `status`/`ready`/`error`/`dirty`/`conflicted`/`busy` properties, with native
  `bindable*()` accessors. Namespace leaf bindings, including `Qt.binding`,
  write through the checked setter and retain normal admission and intent.
- Public error codes `evidence-backlog` for saturated evidence retention,
  `conflict-policy` for invalid source policy, `not-a-draft` for draft-only
  operations, and `overlay-mode` for unsupported writable-overlay operations.
  View-mode refusals remain `overlay-readonly`.

### Fixed

- Keep draft confirmation, whole-source preflight leases and terminal Live handles
  from laundering foreign or replacement selections. Mixed ordinary/group uncertain
  Retry retains causal roots; failed first saves retain their own parent identities,
  and reload migrations use the freshly acquired full revision.
- Keep absent-placement tokens stable and sanitize absolute override paths without
  scope prefixes. Test probes retain asynchronous result storage, and permission
  fault cases check whether DAC restrictions can actually be enforced.
- Replace expanded public revision and projection-run identities with compact
  digests, preserving equality, ordering counters and deduplication semantics.
- Materialize QML signal/callback metadata only when its fields are read, rather
  than charging unused source tables, coverage, publications and evidence to
  every event. Retained events keep their original immutable snapshot.
- Prepare existing namespace getters and bound consumers before public change
  signals across owners, scopes, drafts, previews, overlays and models, without
  depending on connection order. Binding writes retain admission and intent;
  readiness guards also update before closure and detach notifications.
- Own save confirmation no longer invalidates unchanged generation-fenced list
  edits, transfers, container commands or overlay selections. Provenance updates
  remain independently ordered; foreign, other-writer, raw and mapping changes
  still make captured selections stale.
- Admitted open numeric values validate in their declared destination domain.
  Integral overrides now open as Real in every overlay mode, consistently with
  JSON input; drafts, record-list edits, projections and nested values share the
  correction without coercing strings or changing exact raw atoms.
- Drafts and frozen overlay editors can Retry their own admitted failed Apply
  without recapture or duplicate intent. Repaired IO/partial/readback failures
  publish the original work; competing selected changes still conflict, including
  foreign raw edits hidden by pending values. New draft edits admit only their delta.
- Partial worker packets no longer clear retained-group metadata. Complete group
  snapshots distinguish empty from unchanged and reject older publications,
  preserving original-capture Retry after unrelated source errors and diagnostics.
- Uncertain own publications share provenance with watcher admission and Retry.
  Exact own revision/bytes no longer become false external choices or settle
  failed work early; Retry preserves original evidence correlation and newest
  intent. Different revisions, including identical-byte replacements, remain
  external for ordinary and retained work.
- Superseded ordinary PathIntent uncertainty witnesses no longer impose strict
  source recovery. Disjoint foreign edits merge, overlaps report `path-conflict`,
  and later resolution can save without discarding pending intent.
- Keep acquired evidence across retention failures in bounded per-source worker
  backlogs. Saturated sources fail visibly until durable draining succeeds;
  unrelated sources continue, and retries preserve exact bytes and correlation.
  The bound is enforced in Release too; draft/overlay Retry receipts include
  evidence recovered by their internal refresh.
- Overlays publish only changed token/dirty metadata and prepare/notify only
  actual presentation changes. Unrelated publications and duplicate packets are
  silent; invalid mode operations do not introduce busy/conflict transitions.
- On unchanged files, `StrictPendingRebase` now matches `StrictSourcePreimage`,
  including explicit equal-value rewrites and reset of an unchanged absent file.
  No-write shortcuts still apply to actual foreign rebases.
- Unknown numeric members present as exact JS numbers or frozen native
  `.decimal` atoms through namespaces, models, drafts, previews and overlays,
  preserving unsafe numeric tokens on write-back.
- Ordinary save failures no longer prevent unrelated routes from publishing or
  poison later clean saves after resolution. Earlier receipts remain evidence.
- Rejected inputs keep their own failed receipts without failing later publishing
  saves. Autosave can retry resolved ordinary work through the normal policy.
- Strict captures resolve against exact original bytes and presence under the
  lock despite final-file inode/mtime changes. Link and parent identity fences,
  hard-link refusal and late-race checks remain active.

### Changed

- Separate native filesystem revision witnesses from opaque identity. Explicit
  `revision.details()` and retained `revision.json` expose full witnesses;
  conflict, own/foreign observation and evidence retention contracts are unchanged.
- QML adapter event arguments are now `QJSValue` objects with enumerable lazy
  fields instead of `QVariantMap`. QML field access and complete explicit copies
  keep their shapes; C++ adapter connections must accept the new argument type.
  Source errors additionally expose an empty `evidence` array.
- Before 1.0, each minor release has its own shared-library SONAME
  `so.0.<minor>`. The 0.5.0 package installs `libconfigqml_core.so.0.5` and
  `libconfigqml.so.0.5`; native consumers must rebuild on minor bumps.
- Native `Real` admission also accepts integral int/qint64/quint64 values in its
  finite domain, matching JSON numeric admission without coercing strings.
- Autosave retries resolved failed ordinary work through the normal publication
  policy; earlier failed receipts remain history rather than a permanent stop.
- Rebuild native dependents for pre-1.0 ABI changes to `ConfigRevision`, `ConfigPublication`,
  `ConfigFileEdit`, `ConfigJsonTransaction`, `ConfigJsonBackend::reconcile`,
  `ConfigOverlayEditor`, `ConfigDraft`, `ConfigStoreHandle`, `ConfigNumber`,
  `ConfigNode`, `ConfigListModel`, `ConfigStore`, `ConfigQmlStore` and `ConfigQmlOverlay`.

## [0.4.0] - 2026-11-08

### Added

- Record-list match addresses, schema-typed container creation, raw list copy
  with display-equivalence checks, identity ancestor policies and per-operation
  targets. Creation and cross-leaf edits share the existing atomic admission.
- Per-source `unknownData: Preserve | Strict` and Record-only `open` metadata.
  Strict strips unknown data at admission, refuses undeclared members on writes,
  and canonicalizes the next normal publication with warnings and retained
  original-file evidence. Preserve remains the default with unchanged writes.
- QML `sourceWarning.evidence` and native `ConfigError::evidence` expose retained
  original-byte references for Strict publication removals. Open members remain
  excluded from portable profiles.

### Changed

- Rebuild native dependents for pre-1.0 ABI changes to `ConfigSource`,
  `ConfigDescriptor`, `ConfigError`, `ConfigValueSelector`, record-list structs
  and JSON transactions. `ConfigEvidenceReference` is now declared in
  `configbackend.h`.

## [0.3.0] — 2026-10-31

### Added

- Declared record-list transfer moves original rows between lists without losing
  unknown members or persisting normalization defaults. Both endpoint generations
  are fenced; same-leaf and cross-leaf changes admit atomically in one store
  publication, with ordinary per-route save outcomes across shards.

## [0.2.1] — 2026-10-31

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
- Profile exports exclude configured-source aliases, including dangling links,
  and anchor staging/replacement to the same opened parent directory descriptor,
  preventing later parent-path swaps from redirecting publication.

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

[Unreleased]: https://github.com/alexindigo/configqml/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/alexindigo/configqml/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/alexindigo/configqml/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/alexindigo/configqml/compare/v0.2.1...v0.3.0
[0.2.1]: https://github.com/alexindigo/configqml/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/alexindigo/configqml/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/alexindigo/configqml/releases/tag/v0.1.0
