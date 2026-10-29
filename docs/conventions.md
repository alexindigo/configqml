# Core conventions

This is the M1 native contract, targeting C++17/Qt 6.12 on Linux. Production
sources are at the repository root. `configqml::core` links Qt Core only;
test binaries link that production library rather than compiling its sources.

## Ownership and types

`ConfigSchema` takes an owned descriptor tree once. `ConfigDescriptor` is a
construction value; changing it afterward does not change an existing schema.
`Group` is a namespace, with independently merged leaves. `Record` is an atomic
object setting. `List` and `Map` each have exactly one value-owned element
descriptor; records have declared fields, and maps have opaque keys. Recursive
finite descriptor trees are supported to depth 64; cyclic runtime values and
engine-bound objects are not supported configuration values.

| # | Type / presence | Meaning |
|---|---|---|
| 1 | Boolean | Actual bool, not a truthiness conversion. |
| 2 | Integer / UnsignedInteger | Native int/qint64 and uint/quint64 normalize to signed/unsigned 64-bit domains, with no conversion through double. |
| 3 | Real | Finite float/double normalized to double; NaN/infinity are rejected. |
| 4 | String / Url | Owned QString or valid QUrl. Native strings are not silently parsed as URLs or numbers. |
| 5 | List / Record / Map | Owned QVariantList/QVariantMap; QStringList is normalized to a list. Membership types are checked recursively. Unknown record fields remain owned value data, not namespace properties. |
| 6 | Missing | `std::nullopt` for presence queries. Invalid QVariant is undefined/unsupported input, not a persisted value. |
| 7 | Null | `QVariant::fromValue(nullptr)`, only if nullable. Null is never a delete token. |
| 8 | Optional | A setting may lack a schema default; absent overrides then leave it missing. Record fields marked optional may be absent. |
| 9 | Required/defaults | A nonoptional setting needs a valid default. Invalid defaults or descriptor constraints invalidate the schema; there is no tolerant defaults mode. |
| 10 | Choices/ranges | Exact typed enum membership and inclusive numeric bounds. Integer comparisons preserve the full 64-bit range. |
| 11 | Forbidden/sensitive | Forbidden settings reject values. Sensitive metadata excludes data from profiles and redacts input evidence; diagnostic/provenance records contain paths and codes, never value dumps. No secret provider is embedded. See [evolution-secrets.md](evolution-secrets.md). |

Descriptor construction rejects unsupported constraints: namespaces cannot carry
choices, bounds, defaults, optional/nullable or forbidden modifiers; numeric
bounds require numeric, non-null values; fields/element descriptors must match the
node type; list/map elements cannot be optional. Choices are enforced after
recursive value admission on every setting type, including collection values and
explicit null (a nullable enum must list null to admit it). Numeric bounds apply
to the non-null numeric domain. Sensitive is metadata, not a validation predicate.

Values, descriptor constraints, source bytes, path segments and opaque identities
are captured as owned data, including when input QString/QByteArray values use
borrowed storage. `effective()`, `source()` and descriptor queries return detached
value snapshots. `ConfigLayers` is a synchronous single-owner engine: the caller
serializes access, and snapshots can be passed as immutable values. M2 will own
this engine on the ordered worker and publish separate facade caches; do not put
this engine's merge work in a GUI setter.

## Layers, raw data and merge

| # | Precedence | Contract |
|---|---|---|
| 1 | Defaults | Derived once from schema descriptors, not a second handwritten file. |
| 2 | Files | Explicit descriptor order, later file layers win. A logical layer may contain nonoverlapping sources/shards with stable IDs and namespace prefixes. |
| 3 | Environment | At most one explicitly injected layer, after all files. |
| 4 | Runtime | At most one explicitly declared transient layer, last. CLI adapters may feed it; there is no hidden fifth tier. |

Layer identities are unique and `defaults` is reserved. Out-of-order descriptors,
overlapping shard prefixes, duplicate source IDs and multiple writable logical
layers are invalid. A writable layer must be a file-kind layer; zero writable
layers is the read-only case. File-kind is an engine designation in M1, not file I/O.

The engine keeps **raw source data**, original bytes/revision identity, diagnostic
outcomes and admitted leaf projections separately. Unknown keys and invalid
schema values remain in raw input; unknown namespace keys do not become public
settings. A source's `exists` flag distinguishes an absent source from existing
empty `{}`. Original bytes remain load evidence after in-memory edits; they are
not an encoding of the modified parsed raw tree. Storage must encode the edited
raw tree and provide a fresh confirmed revision after publication.

Groups deep-merge by present leaf, including nested groups. Empty group `{}`
contributes no leaves. Lists replace as complete settings, never concatenate or
merge indices. Records/maps are explicitly atomic setting values and replace as
a whole. Thus an admitted empty record `{}` can replace a record with optional/no
required fields, while an empty record missing required fields is invalid.
`false`, `0`, `""`, `[]`, `{}` and allowed null are all values, not absence.

`replaceSource()` validates a new owned source input using the layer's declared
admission policy. These policies apply to source refresh, not to individual
`setUser()` type checks:

| # | Policy | Invalid fields | Raw evidence |
|---|---|---|---|
| 1 | FieldFallback | Omit the invalid source contribution and reveal lower layers. Other valid fields are admitted. | Retain new raw data and all diagnostics. |
| 2 | RetainLastGood | Keep that source's prior admitted value for an invalid field, if available; otherwise fall back. Genuinely missing fields remove the old contribution. | Retain new raw data; old admitted origin stays distinct. |
| 3 | RejectInput | Reject the entire new projection; keep the previous admitted projection, if any. | Retain the rejected new raw data/revision and diagnostics. |

Schema-invalid raw input need not invalidate the store. Unsupported native value
representations cannot be captured as configuration raw data and are rejected
before replacement. Collected diagnostic codes identify paths without echoing
values or parser text. Application resource availability is not a hidden validator.

## Presence, provenance and writes

`info(path)` returns declared/effective presence, winning layer/source, the writable
route, every relevant layer's raw presence/admission/validation outcome and masks.
Each contribution has the **admitted revision**, the separate **raw revision**,
and a `localIntentRevision` for local, unpublished writes. This matters when a
newer invalid raw input is rejected but an older admitted value remains effective.
`info().intentRevision` is the engine's current intent counter, not a backend
publication revision, M2 input sequence or persistence receipt.

Each ordinary assignment targets the named writable layer and the source whose
declared prefix contains the path. The winner is never used to infer a write
target. Source raw maps are relative to their prefixes. Unmapped paths fail
visibly. All explicit path APIs obey namespace/atomic boundaries: an atomic record
is read/replaced at its setting path, not by addressing its internal members.

| # | Operation | Result |
|---|---|---|
| 1 | `setUser(path, value)` | Check/own the leaf, update that user raw override and admitted contribution, preserve unrelated unknown/invalid raw keys, advance intent even for an equal/default-valued choice. |
| 2 | Higher-layer mask | Still retain user intent; getter remains the higher-layer effective value. Write result and info name `maskedBy`; result description is `masked by <layer>`. |
| 3 | `reset(path, targetLayer)` | Explicitly remove that writable layer's override; reveal the next applicable contribution. Namespace reset removes that subtree, including unknown keys explicitly inside it; root reset spans all declared shards. |
| 4 | `setRuntime` / `clearRuntime` | Change only the explicitly declared runtime overlay. Runtime data never becomes user raw data automatically. |
| 5 | Invalid/undeclared assignment | Reject before changing raw/admitted data or intent revision. |

Reset is distinct from assigning null/empty/default. Sparse user state is the raw
override tree, not an effective-tree diff: removing values merely because they
equal today's defaults would lose explicit intent. Editing a leaf does not
re-admit unrelated fields from a previously rejected raw input. M1 returns native
admission results only; it makes no save/completion or GUI notification claim.

## Scopes and plugin sub-stores

Hosts construct `ConfigScope` handles from a shared engine owner and a declared
group path. Each nested scope appends segment arrays; dots, slashes, underscores,
`..`, Unicode and disconnected-device IDs inside a segment remain data. A child
cannot upgrade a read-only parent. Read-only handles reject user/runtime/reset
commands and omit the writable route. Detaching one handle releases its reference
without clearing store state or detaching other handles. Scopes do not import or
reach into their consumers. Independent plugin sub-stores are ordinary independent
engine instances with their own schemas/layers; there is no global registry or
cross-store transaction.

`encodeScopeKey()` produces canonical `k-<lowercase UTF-8 hex>` names.
`decodeScopeKey()` accepts only canonical, reversible, valid UTF-8 encodings.
This avoids traversal and encoding collisions; it is not yet a filesystem path
policy (path length, roots and symlink handling belong to the file backend).
Handles are host-owned conveniences, not an in-process security sandbox.

## Injected environment

`mapEnvironment(schema, mappings, records)` reads only the supplied records, never
process-global environment. Only explicitly mapped declared leaves participate.
Mapping names and paths must both be unique, even when variables are absent.
Strings preserve empty content. Boolean grammar is exactly `true`/`false`;
signed/unsigned decimal integers and finite decimal/exponent reals require the
whole input, without surrounding whitespace, leading `+`, ambiguous CSV or
trailing junk. Overflow fails visibly.

List/record/map mappings use the shared `decodeJsonValue()` boundary, including
explicit `null` when their schema allows it. JSON numbers remain owned
`ConfigNumber` decimal/exponent tokens in raw layers, before binary conversion.
Schema integer admission computes exact decimal integrality and signed/unsigned
64-bit range: `1.0000000000000001` fails with `fractional-integer`,
`9007199254740993.0` becomes exactly signed 9007199254740993, `2^63` is valid only
in the unsigned domain, and overflow fails with `integer-range`. Exact integral
exponents such as `1e3` are admitted as 1000; integer `-0` becomes zero, while Real
`-0` retains its sign. Real explicitly selects finite binary-double semantics;
native floating values are never evidence of integer syntax and cannot enter
integer leaves. Native and scalar environment unsigned integers retain the full
uint64 range. URL mappings
use QUrl strict parsing. Scalar string `null` remains the literal string; null for
scalar native values is supplied as explicit native null, not a guessed env token.
The adapter returns admitted mapped values plus diagnostics; applications retain
the injected source records if they need their original textual evidence.

`environmentMappings()` is an opt-in convenience for lowercase ASCII simple
segments: prefix `APP` plus `["bar", "position"]` becomes `APP_BAR_POSITION`.
CamelCase, underscore-bearing names and opaque identities use explicit aliases.
The generated table passes the same collision/schema check as explicit mappings.

## Backend and codec boundary

`ConfigBackend` declares read/write/watch/conditional-publication capability and
single-owner affinity. Owned requests are registered at `submit()`; `take(id)`
consumes each completion once. No callback is invoked by these core interfaces.
Callers retain the backend until its operations settle. Backend/source revisions,
local intent revisions and request IDs are distinct. `ConfigCodec` separately
declares decode/encode, unknown-key/format preservation, signed/unsigned 64-bit,
URL and nullable capabilities, with structured value-free errors. Read-only or
nonpreserving adapters must not masquerade as writable-preserving codecs.

Numeric codec implementations use `decodeJsonValue()`/`encodeJsonValue()` or an
equally lossless native boundary. The shared helpers preserve original numeric
tokens, including unknown/invalid raw numbers, across encoding and readback;
they do not pass numeric tokens through QJsonValue/double. They are QtCore value
adapters, not file storage or a complete production codec. Decoding a rounded
number upstream into a native integer violates this codec contract: schema
validation cannot reconstruct discarded source digits. Native finite Real values
remain Real and are not implicitly converted into integer leaves. JSON syntax,
duplicate keys and unsupported encoding values are rejected with value-free
diagnostics. The helpers do not claim formatting preservation.

`ConfigMemoryBackend` is per-instance and deterministic. Read exposes presence,
bytes and revision. Publish compares the expected existence/token pair; mismatch
fails without mutation. Successful publication returns confirmed readback bytes
and a new revision. `schedule()` assigns delays/faults to subsequent requests;
`release(id)` explicitly executes a delayed request. There are no sleeps or timer
races. Tests can release in a chosen order; the later worker supplies production
request ordering. `seed()` models external changes/deletion, including revision
changes for missing entries. Foreign-thread controls fail without mutation;
foreign-thread submit returns reserved invalid ID 0.

| # | Fault | Observable outcome |
|---|---|---|
| 1 | BeforeOperation | Read/publish fails before any effect; publication state known. |
| 2 | AfterPublication | Publish changes memory but reports failed/uncertain, with no claimed confirmed new revision. |
| 3 | Readback | Publish changes memory but readback is faulted; failed/uncertain. A later explicit read sees the actual new bytes. |

Memory effects are not file durability, cooperative filesystem locking, watching,
multi-file atomicity or real storage-fault evidence. M3 supplies the first JSON
codec/file backend; M2 supplies the worker, sequence-fenced caches and ordered
three-status save protocol. The M0 native/QML read-your-writes, masked-write,
detached-copy and private-Qt contracts remain downstream obligations.
