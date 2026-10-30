# Ordered store and QML facade (M2)

The Core-only owner/protocol contract is also documented independently in
[worker.md](worker.md).

## Ownership

`configqml::core` remains QtCore-only. `ConfigStoreOptions` contains an immutable
schema, explicit ordered layers, optional store ID and an optional **native,
worker-safe** admission predicate returning a value-free rejection code. Construct
`ConfigStore` on its object thread. One QThread owns its `ConfigLayers`, memory
backend, raw/admitted layers, dirty decisions, merges, diffs and publication.
There are no process-global stores, engine callbacks on the worker, GUI lock
waits or synchronous worker queries.

The object thread owns input captures/sequences, per-path generations, stable
facades, callbacks and caches. `value(path)`/property getters read synchronously
accepted unmasked local writes; `info(path)` distinguishes `latestInput` from
`workerCovered`. `snapshot()` returns the latest immutable **worker-admitted**
generation and its `inputPrefix`, which may precede outstanding local input.
Full snapshots are built and replaced snapshots reclaimed on the worker; mutable
GUI cache containers are never shared with snapshot consumers. `inputs()` and
`failures()` are explicit diagnostic/evidence copies, not the save capture API.

Native callers use segment-array paths, e.g. `store.setUser({"bar", "position"},
QStringLiteral("top"))`, then `store.save(context, callback)`. All store access is
on the store's object thread; immutable snapshots may cross native threads.
Callbacks with another living QObject context are queued to that context's thread.

## Ingress and publication

Every property write enters C++ before equality suppression. The shared setter
boundary owns supported values, rejects invalid type/choice/range/undefined/
nonfinite/cyclic/engine-bound input visibly, allocates its sequence, registers
its callback and queues the owned command before installing its local value and
notifying consumers. Equal assignment remains explicit user intent even when it
does not emit NOTIFY.

The worker authoritatively admits or rejects the event. Delta publications carry
topology, per-path generation, covered input and publication-generation fences.
Stale replies can finish their correlated request but cannot replace newer local
input or its masking provenance. All eligible caches in a grouped publication
are installed before the first notification. Rejection of the latest write
restores authoritative effective values before failure signal/callback delivery.
Publications never enter ingress as new persistent events.

Every command settlement, including terminal-worker refusal, covers the input's
sequence and path generation. An outstanding latest local write therefore rolls
back to authoritative state before its failure callback; stale failures still
cannot erase newer pending input. Retained evidence is separate from getters.

A known higher-layer mask keeps the effective getter unchanged. For example,
masked `push(4); push(5)` over `[3,1,2]` records `[3,1,2,4]` then `[3,1,2,5]` in
the user layer, with `maskedBy` and `description: "masked by <layer>"`. Unmasked
appends instead compose to `[3,1,2,4,5]` immediately, before worker replies.

## Requests and lifetime

Native `setUser`, `setRuntime`, `reset`, `clearRuntime` return a
`ConfigRequestHandle`: synchronous `accepted` means checked **local ingress**,
not worker admission or persistence. Invalid native input creates no input event
and also receives a later failed request result if a callback was supplied.
QML conversion/schema errors throw synchronously and emit `inputRejected`, with
no accepted event or dispatch. Native Qt property coercion happens before a typed
setter: an `int` property can receive string `"4"` as 4, and `QList<int>` can
receive `"oops"` as 0. The library validates what actually reaches that setter.

Save/flush/close results have exactly three statuses:

| # | Status | Meaning |
|---|---|---|
| 1 | `saved` | Required memory publications and readback completed for the captured prefix. |
| 2 | `nothing_to_save` | The worker found no covered pending persistent work; still asynchronous. |
| 3 | `failed` | Correlated admission/publication/readback/worker error; failure and intent remain retained. |

Results include request/store identity, operation, input prefix, confirmed covered
revisions, per-source publication outcomes, known/uncertain/partial state and a
structured value-free error. An uncertain route is never called saved. Partial
success remains failed, with each route's actual state. Later save/flush cannot
silently retry or erase a retained rejection or failed attempt.

`save()` captures the reported sequence prefix, never the facade graph. A→B→A
across save boundaries produces three ordered captures/publications, not one
coalesced value. Every completion is asynchronous, signal before callback, and
delivered in request order. The callback is detached before consumer reentry.

`flush()` publishes its captured prefix and awaits earlier work. `close()` seals
ingress at that prefix, flushes it, then releases the worker after ordered
completion. Later writes fail visibly. Read-only stores never write on these
requests. `x=1; close()` publishes x=1; empty flush returns nothing-to-save; flush
after a failed save returns failed and retains evidence. Memory backend delays
hold the request queue without blocking the GUI. Unexpected worker exceptions or
protocol failure stop persistence visibly; the owner is never recreated/replayed.

Use asynchronous close when a caller needs a publication receipt before continuing.
Its completion includes actual worker-thread teardown, so normal exit after the
callback cannot race thread-local cleanup. Destroying the consumer cancels only
its callbacks: accepted persistence with autoSave or a pending save/flush/close
is handed to the detached worker, retained by the process shutdown registry.
The one process-level event drain, registered for aboutToQuit and post-routine
exit, is independent of store QPointers and handles deletion during shutdown.
Its shared five-second budget and stderr error reporting are documented in
[json-files.md](json-files.md). With autoSave false and no outstanding persistence
request, manual unsaved edits are not implicitly saved. The process itself must
remain alive long enough for settlement; abrupt process termination cannot flush.

## Import-only declarations

URI **ConfigQml 1.0**, target **configqml::configqml**, production dependencies
QtCore and QtQml (the Arch QtQml binary also links QtNetwork). No Quick/Gui/Widgets/
DBus/Concurrent production dependency. The private `QMetaObjectBuilder` adapter
is isolated to `confignamespace.cpp`. Configure requires exact matching Core,
CorePrivate and Qml **6.12.0** and writes `configqml-qt-build.txt`; update/rebuild
the adapter deliberately when Qt changes. This is not a public-Qt-only module.

`ConfigSchema` and `ConfigGroup` accept descriptor children through a default
QQmlListProperty. A group has a key; `ConfigValue` supplies key, type, default,
choices, bounds, optional/nullable/sensitive/forbidden metadata. Declarations and
sources compile once into owned descriptors at component completion. Subsequent
declaration edits do not change the store. Namespace QObject identities and
metadata are immutable; namespace edges are nonwritable. Atomic records/maps are
settings, not namespace merges. Lists/maps contain one unnamed ConfigValue
element declaration; records contain keyed field declarations. StringList is a
convenience for a list of strings. No application schema plugin is needed.

| # | QML valueType | Native descriptor/domain | QML representation |
|---|---|---|---|
| 1 | `Boolean`, `String`, `Real`, `Url` | bool, QString, finite double, QUrl | Ordinary corresponding Qt/QML values; URLs require a URL value such as `Qt.resolvedUrl(...)`. |
| 2 | `Integer` | `SafeInteger`, −(2^53−1)…2^53−1 | JS number; fractions/out-of-range fail in every native/raw/environment admission path. |
| 3 | `Int64`, `UInt64` | Existing native `Integer`/`UnsignedInteger`, qint64/quint64 | Decimal strings only, consistently even for small values and inside records/lists/maps. Numbers, overflow and invalid decimal syntax fail visibly. |
| 4 | `List`, `StringList`, `Record`, `Map` | Owned lists/atomic records/opaque-keyed maps | QVariant values with schema-directed recursive conversion. |

M1 native `Integer` remains exact signed 64-bit; native callers select
`SafeInteger` for the safe-number domain. JSON numeric tokens retain exact syntax
until schema admission. M2 uses memory bytes, not a JSON/file codec.

Persistent write capability is compiled from immutable source prefixes. A leaf
without a writable user route is a nonwritable dynamic meta-property. Direct QML
assignment fails visibly, and explicit path writes reject synchronously without
an accepted input or unsaveable local projection. Native static Q_PROPERTY
facades use the same ingress capability check. Read-only flush/close remain
asynchronous and perform no publication.

`MemorySource` declares layerId/sourceId, source prefix, raw data, existence,
kind (File/Environment/Runtime) and writable flag. Entries sharing a layer ID
must agree on kind/writability. Sources/layers keep declaration order; all M1
topology/precedence constraints apply. No sources means read-only. QML-authored
declared source values must pass the QML type boundary or initialization reports
Error; unknown raw keys stay owned. Native raw layers retain their M1 admission
policies. Memory transport is an internal tagged native-value envelope, not a
stable file format or disk durability claim.

QML exposes `values`, `ready`, `status` (Loading/Ready/Error/Closing/Closed),
`error`, `info(path)`, `setUser`, `setRuntime`, `reset`, `clearRuntime`, `save`,
`flush`, `close`, `completed(result)` and `inputRejected(path, code)`. Optional
callbacks are ordinary JS functions. Request IDs/prefixes/generations in the
QML result/info maps are decimal strings to avoid eventual counter rounding.

For application-authored native Q_PROPERTY facades, derive from `ConfigFacade`
and forward getters/setters to `readSetting(relativePath)` and
`writeSetting(relativePath, value)`. Route `settingChanged(relativePath)` to the
matching declared NOTIFY signals. This base shares the checked ingress and has
no second cache/merge owner. It emits synchronous `inputRejected` on invalid
input; an associated QML engine also receives a TypeError. Use QVariant properties
for exact-wide string domains when retaining source-type rejection matters;
a typed QString setter may already have coerced its argument before ingress.
Core-only native clients may forward directly to
ConfigStore instead, without linking Qml.

All library boundaries that notify consumers or invoke callbacks use the same
Core-only `ConfigConsumerGuard`: grouped facade and namespace notifications,
rejection signals and subsequent TypeError reporting, store publication/status/
completion, QML forwarding and JS/native result callbacks. It checks QPointer
lifetime on the caller's stack; continuation stops after destruction. Metadata
dispatch also snapshots its local property count before invoking a setter, so
it does not read a destroyed namespace afterward. Signal arguments/callback
records are owned or detached before reentry. These guards preserve synchronous
notification and coherent publication; they do not defer it to a timer.

## Value-copy and tooling boundaries

Property-origin list/index/length/mutator edits, attached JS local references,
atomic record nested edits and namespace leaf writes reach real setters. A QML
`property var/list` copy, component copy or raw Repeater modelData is detached:
edit it and assign it back to change the setting. No ordinary-copy apply API,
save-time scan or hidden synchronization exists. Consumer detached-copy lint is
separate tooling; [read-only collection models](models.md) provide protected
display projections. [Explicit drafts and scopes](editors-profiles.md) provide
separately named editor previews without changing ordinary-copy semantics.
Unknown dynamic JS properties are not admitted settings; explicit path APIs
reject undeclared keys. Runtime schema leaves cannot all appear in static
qmltypes, so dynamic consumers use a var handle; native facades supply static
metadata. The test fixture's eval is intentional driver code, not production.

## Verification lanes

CTest includes native protocol/fault/affinity/timing assertions, cold QML engines
with actual bindings for dynamic/native facades, an independent QML-only process
that links no configqml library, Core-only dependency checks, and private-API
isolation/exact-version checks. Timing reports worst GUI ingress and publication
microseconds; it is characterization, not a promised full-shell 16 ms budget.
File/watcher conflict behavior and collection models are documented in
[json-files.md](json-files.md) and [models.md](models.md). Drafts/profiles are
documented in [editors-profiles.md](editors-profiles.md), and migrations/admission/
secrets in [evolution-secrets.md](evolution-secrets.md). Installed-package
compatibility remains the distribution milestone.
