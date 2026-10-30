# Schema evolution, admission and secrets

## Versioned raw migrations

Native `ConfigSchema(root, version, migrations)` accepts ordered
`ConfigMigration{fromVersion, transform}` functions. Each returns a
`ConfigValueResult` containing a plain raw QVariantMap or errors. Captures must be
owned and worker-safe. No application QObject, engine-bound callback or external
application effect may be captured. Steps are selected by fromVersion and run in
ascending order; a missing/duplicate/out-of-range step fails visibly.

QML declarations accept standalone **JavaScript source strings**:

```qml
ConfigSchema {
    version: 1
    migrations: [{fromVersion: 0, transform: `function(raw) {
        if (Object.prototype.hasOwnProperty.call(raw, "oldName")) {
            raw.newName = raw.oldName
            delete raw.oldName
        }
        return raw
    }`}]
    // declarations...
}
```

Source strings are construction-time owned data. Each transform is compiled and
called in a fresh **worker-local QJSEngine**, without the declaring QML context,
closures, imports, injected QObjects or application globals. A live QML function
object is rejected: Qt 6.12 function.toString emits `[native code]`, not source.
Transforms must be bounded synchronous plain-data functions. Exceptions and
unsupported/cyclic output become value-free migration errors.

Every raw map crosses this bridge as a **null-prototype dictionary**, created
with `Object.create(null)` and populated through own data-property definitions.
Any string key, including `__proto__`, `constructor`, `prototype`, empty names
and names of Object methods, is ordinary data at every nesting level. Bracket
and dot access/assignment remain natural; use
`Object.prototype.hasOwnProperty.call(raw, key)` for own-key checks rather than
assuming maps inherit methods. An identity transform reproduces the raw input
exactly, including unknown keys and exact numeric lexemes, apart from the
library-owned version stamp.

Exact raw JSON numeric tokens enter JS as frozen plain objects with a `decimal`
string. Copying/renaming these objects preserves the exact token on return by
object identity, including unknown uint64 values; no shape-based magic key can
collide with ordinary raw objects. To calculate, an application can explicitly
choose `Number(raw.field.decimal)` within the domain it intends. New JS numbers
use finite binary-number semantics and are returned as JSON number tokens before
schema admission. Returning a string remains a string. Do not stringify/clone a
token wrapper when intending to retain its numeric identity.
Native memory-source numbers use the same representation, preserving exact wide
integers and negative zero; native URL atoms use frozen `{url: string}` records
with identity-preserving return. JSON file URLs remain ordinary raw strings.

Each declared file/shard has reserved raw `_configqmlVersion` metadata, absent
meaning zero. The schema reserves that key. Transforms receive the source-relative
raw object without that metadata and must preserve unknown entries they are not
deliberately renaming/removing. The same step sequence applies to every old-version
shard; guard renames by raw presence. Schema version zero disables migrations.
Future/invalid versions and read-only sources needing migration fail; read-only
stores never persist migrations. Missing optional sources are not created merely
to stamp a version.

At startup all transforms are prepared before any required publication. Exact
original bytes are retained in 0600 `<resolved-target>.configqml-v<N>.bak` files;
existing conflicting backup evidence is never overwritten. Transformed roots use
the same fresh-read, cooperative-lock, resolved-symlink-target, atomic publication
and confirmed-readback backend as normal writes. Unknown keys/numeric tokens
survive preserving transforms. The version stamp is part of that publication,
never a premature in-memory completion flag. Ready follows required persistence
and admission. `migrationCompleted` reports saved/nothing_to_save/failed with
covered revisions and per-route/partial/uncertain outcomes. Transform failure
leaves original files unchanged; publication/readback failures retain backups and
report what actually reached disk. Multi-file atomicity is not promised.

The worker-owned JSON backend exclusively creates private 0600 same-directory
staging from its first open, writes the complete original bytes, flushes and syncs
the file, then publishes the backup name atomically without replacing evidence.
It syncs the parent directory before any source replacement. Exact, private regular
backups reused on retry cross the same file and directory sync barriers. A failed
unpublished attempt removes only its owned staging; cleanup failures report that
staging path. Complete published backups survive all subsequent failures. Existing
conflicting or non-private evidence is refused unchanged. These local-filesystem
ordering guarantees do not claim universal power-loss safety.

Explicit/live reloads run that same migration owner before admission. Old-version
foreign data cannot be migrated over outstanding local edits: that case reports
`migration-pending-intent`, retaining the old baseline and pending work. An
application needing credential migration or external file/domain moves coordinates
those effects itself; transforms operate on the named store's raw shard content.

## Per-node source admission

`ConfigValue`/`ConfigGroup.admissionPolicy` is Inherit (default), Strict or
Tolerant. Native metadata is `ConfigDescriptor::AdmissionPolicy`. Inheritance
follows groups and nested record/list/map descriptors. Inherit uses the source
layer's policy; `shippedDefaults: true` selects strict by default, while user
sources default to tolerant FieldFallback. Invalid schema-authored defaults always
invalidate the schema. Declaration metadata is frozen at construction.

Tolerant invalid fields fall back to the next lower contribution while keeping
the new raw value and diagnostic. Strict invalid fields reject the **whole layer
projection**. Batched shard reloads preserve the entire prior admitted layer,
including otherwise-valid sibling shards, while retaining every new raw source.
At initial load a strict failure prevents Ready and contributes no part of that
layer. Existing source-wide RetainLastGood remains available under inherited
policy; an explicit Tolerant node selects fallback instead. Ordinary setter
schema validation is always rejecting, never tolerant coercion of invalid input.
`MemorySource.admissionPolicy` and `JsonFileSource.admissionPolicy` use the same
source policies. The QML adapter owns supported raw input and normalizes valid
QML numeric domains; owned invalid fields are retained for ConfigLayers to admit
or reject. ConfigLayers records the actual rejection, and initialization consumes
that fact for native raw, QML memory and JSON alike. Inherited RejectInput never
reports Ready merely because a source did not use a JSON adapter.

Provenance exposes presence, admission validity, source/mask identities and
revisions without embedding values. `contentRevision` is a worker-owned per-path
raw/effective/intent revision used by drafts, distinct from whole-file storage
identity, ingress generation and publication generation.

## Credentials remain application-owned

`sensitive: true` is an information/export boundary, **not encryption**. It is
inherited from groups and excludes atomic settings containing sensitive
descendants from profiles. Diagnostic/provenance/result codes contain no values;
the explicit native input-evidence API redacts sensitive/SecretRef values too.
Ordinary getters and immutable consumer snapshots still deliver declared values
to authorized in-process consumers. File backups and raw persistence retain their
original contents; marking data sensitive cannot undo an earlier plaintext file.

All diagnostic producers share `ConfigDiagnosticBuilder`. Schema-backed capture,
numeric/JSON codecs, environment/layer admission, native/scoped rejection,
reload/source errors and QML result serialization bind immutable schema metadata
to that builder. Below a sensitive node (including inherited groups and sensitive
elements), content-derived map keys, list indices and unknown record members are
reported as **`<redacted>`**. Authored schema field names and value-free codes
remain useful; values are never interpolated into errors or exceptions. Detached
handles retain this metadata for rejection diagnostics after their owner detaches.
Native results and QML serializers use the same builder defensively at delivery.

Low-level standalone capture/codec helpers also accept a diagnostic builder;
callers processing schema-backed data pass `schema.diagnostics()`, or `.at(prefix)`
for source-relative data. Context-free codecs cannot infer an application's
sensitive schema. The library's store adapters always supply that context. The
builder distinguishes local source paths from already-constructed absolute
diagnostics so repeated propagation does not duplicate prefixes or expose keys.

`ConfigValue.SecretRef` / native `Type::SecretRef` admits only an opaque string.
Its grammar, resolution, lifetime and provider belong to the application. It is
automatically excluded from profiles and redacted from input evidence. Store an
app-owned keyring reference such as `keyring:account-id`, keep the actual credential
in the OS keyring, and resolve it through the application's secret provider.
`forbidden` rejects credential fields that must never enter declared settings.
There is no keyring implementation, vault, authentication flow or credential
migration inside configqml.
