# Owned retention and synchronous input

Accepted values, schema metadata, request paths and retained map/cache keys are
owned Qt values. Copying a QString/QStringList that wraps `fromRawData` is not an
ownership capture: that wrapper still follows the caller's character storage.
`ownString` and `ownPath` establish this boundary when a query/command identifier
is retained. Pure lookups borrow their arguments for the duration of the call.

## Retention audit

The following describes production key provenance and the audited boundaries.

| # | Retained state | Owned key source / capture |
|---|---|---|
| 1 | Core presentation cache | A miss inserts ownPath(query). A warmed getter performs only a lookup, without copying the collection or recapturing its key. |
| 2 | QML list/keyed-model handles and rejection callbacks | Query paths are captured at entry before caching or retaining them in callbacks. |
| 3 | Local values/information, input path-generation maps and worker inputs | Core write entry owns the path before constructing any map or event. Sealed schema-admission paths are already owned; the Core boundary preserves that guarantee. |
| 4 | Core model/write-route maps | Models use immutable schema leaves; routes use construction-time owned source prefixes and IDs. |
| 5 | Worker paths, dirty/revision/source maps, model-version/plan/ack maps | Keys derive from owned schema, captured input and construction-time owned options. No caller query storage is retained by this protocol. |
| 6 | Dynamic property dispatch registry | Namespace metadata comes from the frozen Core schema. Names and namespace paths are explicitly captured; later mutation of QML declaration buffers cannot redirect property reads. |
| 7 | Layer admitted-path maps and source registries | Schema leaves and ownPath writes provide map keys. Source IDs/prefixes are owned even on invalid-source initialization paths. |
| 8 | Memory backend entries and queued requests | seed/submit capture source IDs and bytes before storage; completion maps use primitive request IDs. |
| 9 | JSON backend descriptors, read counters, confirmed source/target caches and results | Constructor owns file paths/IDs; submit/reconcile own request IDs; read/confirm retain owned string keys. Results retain the captured request identity. |
| 10 | JSON layout/order metadata | decode owns its root path before producing layout keys. Decoded object keys are owned parser values. |
| 11 | Row-ID index hashes and protected nodes | IDs/keys derive from admitted owned rows; nodes retain only immutable owned schema/value data. Public node construction uses the schema-owned factory. |
| 12 | Process shutdown registry | Keys are primitive QObject pointer identities; shared worker owners own their store IDs. No borrowed text key is stored. |
| 13 | Drafts and asynchronous scopes | Draft paths derive from the owned schema; edits use canonical owned validation/sealed admission. Scope prefixes and profile command paths/names are captured before retention. Detaching consumers cannot cancel worker-owned groups. |
| 14 | Raw migrations | Native callbacks own worker-safe captures; QML compiles explicit owned source strings in a worker-local engine. Only captured plain data leaves that stack frame. Original bytes/backups and confirmed publication revisions remain worker-owned. |
| 15 | Diagnostic schema context | ConfigDiagnosticBuilder retains immutable descriptor metadata and owned base/source prefixes. One implementation identifies authored versus content-derived segments; capture, codecs, layers and native/QML delivery share it. Detached scopes retain the context, never a raw value/merge owner. |
| 16 | Accepted intent fences | The worker advances one per-path accepted-intent revision for every successful command/group producer. Object-thread path generations remain ingress fences, and cannot be synthesized by a worker-built profile group. |

Environment/descriptor compilation has temporary lookup tables over its inputs;
only owned output paths/values and the frozen schema cross the compilation
boundary. ConfigDescriptor remains an application construction value; ConfigSchema
owns its immutable compiled metadata. `putValue` also owns inserted path keys.

Regressions cover the `list` → `mist` borrowed-key mutation (including wrong-type
cache lookup), cache-key storage lifetime, held-write path generations, list and
keyed QML model handles, frozen dynamic property names, JSON layout paths, and
direct JSON backend construction/request/confirmation IDs.

## One owning validated traversal

`ConfigValueInput` is a synchronous, non-retained input view. It reports scalar/
list/map shape and visits children; the caller keeps the source alive for the
call and does not retain the visitor. The native view wraps QVariant data. The
QML view wraps JS/native Qt values, checks ancestor cycles and engine-bound types,
and constructs no intermediate owned tree.

Core's schema-directed traversal builds the final owned containers and validates
each child while reading it. It enforces shape, optional/nullable/forbidden fields,
required members, row-ID uniqueness, numeric domains and constraints. A purported
scalar carrying a composite QVariant is rejected rather than trusted as an
already-validated subtree. Error paths and stored keys remain owned. Schema field
names can share the schema's immutable storage; opaque input keys are copied.

`ConfigSchema::admit(path, input, format)` returns a sealed ConfigAdmission. It
has no public unchecked-value constructor or mutable payload. Schema identity
and an owned path/value are retained; default or foreign-schema admissions are
refused by a store. Returning a copy from value() cannot mutate the token.

QML facades capture through a local immutable schema copy under the existing
lifetime guard, then pass the token to the canonical Core setter. Input property
getters may reenter or destroy a consumer during capture without making Core's
write continuation access a destroyed QObject. No input view, engine value or
engine callback is queued to the worker. Core still owns sequence allocation,
write-capability/mask checks and queue-before-notify order; the worker remains
authoritative for admission, layers and persistence. Schema acceptance does not
claim that a write has been admitted by the worker or saved.
