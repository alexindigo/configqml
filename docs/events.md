# Compact event identities and lazy metadata (0.5.0)

Library-generated JSON and memory `ConfigRevision::token` values are opaque
64-character SHA-256 identities, not serialized paths or revision tables.
Projection input/event `revision` and effect `runId` are likewise fixed-size
digests. Equality, source/presence identity, projection-run deduplication and
own/foreign observation distinctions are unchanged. Digest tokens have no
ordering meaning. Request IDs, input prefixes, event counters and generations
retain their numeric ordering and decimal-string QML representation (at most
20 digits for a 64-bit counter). UUID group, attempt, evidence and editor IDs
remain compact. Application-authored names and request keys are not rewritten.

The JSON backend keeps an immutable structural witness separately from the
token. Target paths, stamps, content digests and parent/link identities still
govern preimage comparison, own-created-parent advancement, confirmation,
watch detection and late-race checks. Native `revision.details()` explicitly
returns this metadata; it is empty for revisions without a filesystem witness.
Retained evidence keeps its compact correlation token in `metadata.json` and
the complete filesystem witness in a separate owner-only `revision.json`.
Original bytes, correlation, retention and failure semantics are unchanged.

## QML event access

Store and overlay completion signals, autosave/migration receipts, source
accepted/effects events and source diagnostics now deliver engine-local
`QJSValue` objects instead of native `QVariantMap` arguments. Existing QML
property access is unchanged, including arrays, map keys and nested values:

```qml
onProjectionEffects: function(event) {
    for (const effect of event.effects) {
        applyEffect(effect);
    }
}
onCompleted: function(result) {
    if (result.status === "failed")
        reportFailure(result.error.code);
}
```

Each field is an enumerable lazy property. Its first read materializes only
that field from the immutable native snapshot, then caches the detached value
on the event. Reading `status` does not build covered revisions, publication
routes, coverage or evidence; reading `effects` does not convert the IDs or
source tables. No detail accessor consults a newer store generation or waits
for the worker. Retaining the event keeps its original snapshot, including
after the emitting store is destroyed. Engine garbage collection reclaims
unreferenced snapshots; there is no growing store-owned event registry.

Explicit inspection/copying such as `JSON.stringify`, `Object.assign` or
enumeration can materialize metadata. In Qt 6.12, key enumeration also visits
accessor values. Large fields still cost proportionally when requested; this
is not truncation, a cap on evidence, or a full-application frame-time promise.
Event fields can be overwritten locally without changing the native snapshot.
`sourceError` also exposes an empty `evidence` array, as warnings already do.

## Native compatibility

This changes the native ABI of earlier development builds of the unpublished
0.5.0 release: rebuild consumers for `libconfigqml_core.so.0.5` and
`libconfigqml.so.0.5`. C++ connections to the QML adapter's map signals must
accept `const QJSValue &` instead, using `toVariant()` for an explicit eager
copy if required. These values belong to their QML engine and object thread;
they are not cross-thread native event transports. Core-only users keep the
typed native results and their implicitly shared metadata, without QtQml.
The explicit `qmlResult()` conversion and on-demand `sourceStates`/`groupInfo`
queries still return their complete native map/list representations.
