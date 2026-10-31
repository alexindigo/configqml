# Declared nonpersistent views

Source projections, conditional file fallback and record-list policies run in the
ordered worker. They do not introduce effective-value scripts, write routes from
provenance, or implicit persistence. Construction declarations are frozen with the
store; edits to their QML objects afterward do not reconfigure it.

## Source transforms

`ConfigStore.projections` accepts `ConfigProjection` declarations with a unique
`projectionId`, explicit nonempty `sourceIds`, owned dictionary `metadata` and a
standalone function-source string `transform`. Native callers supply
`ConfigProjectionSpec` with a worker-safe native callback. The callback receives
`raw`, `source`, `schemaVersion`, `priorVersion`, `defaults`, `lowerValues`,
`values`, `compatibilityRaw` and `metadata` as specified by `configprojection.h`.

Source scripts use a fresh worker-local engine, never a GUI closure. Dictionaries
are null-prototype, and payload numbers/URLs use the same identity-preserving
frozen atoms as durable migrations. Controls such as schemaVersion remain ordinary
numbers; revisions are strings. Return exactly `{values, compatibilityRaw,
effects, diagnostics}`. Each root is a dictionary; effects and diagnostics are
arrays. Copy a numeric atom to retain its exact domain/lexeme; explicitly read
`.decimal` to perform arithmetic. A dictionary that merely looks like an atom is
ordinary data. Missing and null remain distinct.

Transforms see only their source and its lower context. They run in declaration
order before schema admission, and repeat when their raw candidate or lower
context changes. `compatibilityRaw` flows to the next transform independently of
`values`. Neither becomes writable raw. Ordinary edits still encode the original
raw tree plus accepted user intent, preserving unknown keys and read-only source
bytes. Durable versioned migrations remain a separate mechanism.

`projectionEffects(event)` follows cache installation and precedes Ready/completion.
The store deduplicates each projection/source/accepted-observation identity;
applications execute the returned actions and own their effect failures. This is
not a durable exactly-once protocol. Unsupported, cyclic, nonfinite, too-deep or
malformed output fails with value-free projection codes.
Nonfatal authored diagnostics use `sourceWarning` (native `ConfigError`, QML the
same structured code/sourceId/phase/file/path/line record as failures), with phase
`projection-warning`. They follow cache installation and do not prevent Ready or
successful completion. `sourceError` is reserved for failures; applications need
not inspect its phase to distinguish a successful admission from a warning.

## Conditional fallback

`JsonFileSource.fallbackFor` names a later File-kind source whose namespace covers
the fallback's prefix. The fallback must be read-only; chained, backward, unknown
and self selections are invalid. Native declarations use `ConfigSource::fallbackFor`.

The worker counts immediate keys in the target's **unprojected raw subtree**.
Absent or `{}` enables the entire fallback. Any key, including unknown/invalid or
version metadata, suppresses its entire contribution. Nonobject subtrees and
unknown/unreadable observations are not evidence of emptiness. There is no
per-leaf fallback and no adoption write. Namespace reset removes user raw choices;
once no keys remain, fallback returns without copying its data to the user file.

## Source presence and accepted facts

File sources default to `FailReady`. `SkipProjectionRetainError` permits unrelated
sources to become Ready after startup parse/IO failure, retaining original byte
evidence in the required absolute clean `evidenceDirectory`. It quarantines that
route against writes until a successful repair read. Schema/transform failures
are not bypassed. Runtime IO/parse failures preserve last-good admission.

`sourceStates` reports ordered metadata, presence, load outcome, accepted revision,
quarantine, fallback selection, value-free diagnostics and evidence references.
It is not a raw settings mirror. Evidence directories/artifacts are owner-only,
unique and synced before references are issued.

`observedPaths` selects declared leaves for `sourceAccepted(event).facts`.
These facts describe the accepted disk-layer branch, independent of pending local
display values, environment/runtime masks and record-list normalization. Origin
and cause distinguish startup, foreign reads, own saves and metadata projection.
An unchanged confirmed own echo generates no acceptance event. Metadata-only
events do not pretend to be an external settings choice.

## Record-list housekeeping and editing

`ConfigRecordListPolicy` names one atomic owning leaf, a declared `listPath`, its
record `keyField`, `preservedFields`, and optional pruning/normalization. Selectors
are `{key: string}`, `{index: integer}`, or policy-only `{each: true}`. Overlapping
policies on the same leaf are refused. The library has no plugin-name convention.

`updateRecordListPolicy(id, {ready, entries}, callback)` supplies owned registry
facts. Each entry has `key`, `available`, `retain`, `normalize` and dictionary
`defaults`. Until ready, the policy is identity. Pruning removes unknown records
or records whose entry is neither available nor retained. Normalization removes
unrecognized fields and fills absent defaults, preserving keyField and explicitly
preserved fields. Present false/zero/null/empty values are never replaced. Metadata
updates, including equal updates, advance their own revision and create no user
intent or persistence. The worker always recomputes from unnormalized values.

Use `editRecordList(id, {generation, listAddress, edits}, callback)` for editing a
normalized view. `generation` is the displayed leaf's
`info(path).publicationGeneration` string; listAddress is concrete (no Each).
Supported operations are set/unset field, remove, insert and move, with indices
interpreted sequentially against the displayed list. Worker row mapping directs
each operation to raw rows. Hidden rows and deprecated raw fields survive an
unrelated field edit; injected defaults are not copied into raw. Stale/unsettled
addresses fail. One owning-leaf replacement is admitted atomically; a later
ordinary save/autosave persists it. The admitted receipt is not a save receipt.

Ordinary whole-leaf assignments retain their existing meaning: the whole submitted
value is explicit user choice, not an inferred diff of a detached display copy.
Property/model generations install coherently before completion. Unrelated path
publications do not invalidate a displayed leaf's address generation.
