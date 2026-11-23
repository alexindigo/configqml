# Generic editing and observation contracts

The initial native/QML slice shares selections, explicit edits and causal read
observations beneath the existing facades. It is not a second settings engine.
ConfigLayers still owns layer resolution; store workers own candidates, leases,
admission and persistence; the existing publication boundary owns notifications.

## Current slice and subsequent adoption

| # | Contract | Initial routing |
|---|---|---|
| 1 | P1: owned selection | Declared leaf/group/disjoint-union validation, schema identity, target and lifetime are captured once. Atomic Record/Map/List members are not new storage paths. Checked overlay addresses retain their existing mapping/identity fences. |
| 2 | P2: explicit batch | Omitted leaf means no edit. Set and RemoveOverride remain distinct from null, absence and equal intent. Draft and raw-slot staging/admission use one candidate resolver over frozen or current layers, including source fallback and record-list projection. |
| 3 | P3: causal observation | Every installed capture retains actual worker dependency cuts, selected target and preview lifetime/epoch where applicable. Completion follows installation, bindable/leaf notification and legal model updates of that exact reader. |

The recommended interface is the schema-generated handle graph below: generated
handles, one verb vocabulary and one pending reply. It adapts these same
contracts; it is not another capture owner or settings engine. The `readProjection`
callback API further below remains supported as a compatibility recipe. Existing
methods, values, numeric domains, namespaces and persistence/Retry behavior
remain valid.

## Generated handles

Every owner exposes a stable `root` handle generated from its schema: a store's
root is live (user layer), `runtimeRoot` is live on the runtime layer (no disk
route), a draft or overlay editor's root is staged, and a read-only scope, overlay
view or draft preview is read-only. Native owners provide nonvirtual `root()`
(and `ConfigStore::runtimeRoot()`); QML owners provide `root` (and `runtimeRoot`)
properties. A new setting or plugin needs a schema, not new library methods.

| # | Access | Meaning |
|---|---|---|
| 1 | `h.scopes.general`, `h.fields.scaleRatio` | Generated Group and field handles, including optional defaultless fields before a value exists. QML lookup objects have no prototype, so any schema key is plain data and never shadows a verb. |
| 2 | `h.scope(["general", "scaleRatio"])` | Cached lookup by opaque segments; dot access is convenience only. An undeclared path or an atomic Record/Map/List member gives an invalid handle; it never widens to its parent. |
| 3 | `h.select([["general"], ["bar", "position"]])` | Disjoint selection within the same owner and permission; Groups expand to all declared fields, including undisplayed ones. |
| 4 | `h.value`, `h.present`, `h.values` | Binding inputs. Live: checked read-your-writes value. Staged: the checked candidate (inheritance after a staged removal), the authored value while a set is unresolved, nothing while a removal is unresolved. Read-only: the owner's projection. `value` notifies exactly once per actual effective transition, including absent/present; equal intent and metadata-only publications do not notify. |
| 5 | `h.state`, `h.descriptor` | Immutable selected state (target presence, pending `none`/`set`/`remove`, authored, candidate, mask) and declared metadata. Neither is a widget or codec. |
| 6 | `h.generation`, `h.capabilities`, `h.valid`, `h.health`, `h.kind` | Opaque installed observation, supported verbs, owner/selection lifetime, last acquisition health (`ok`, `failed`, `invalid`) and `live`/`staged`/`readOnly`. |

Handles are materialized lazily and only materialized handles are notified;
getters never capture, resolve a candidate or touch disk.
Lookup first validates and narrows through the existing selection owner. Its
root-local cache identity contains the lookup kind, namespace, canonical allowed
leaves and requested paths as typed containers. Opaque segment data cannot alias
path boundaries; a narrowed selection never reuses a wider handle, and an invalid
lookup cannot poison a valid one. Equivalent validated lookups remain stable.

The shared selection validator indexes allowed path ancestors once and expands
requested namespaces through that index, retaining canonical schema leaf order.
A full-leaf capture does not compare every requested path with every allowed
leaf; opaque segments, duplicate handling and narrowed capabilities are unchanged.

### Verbs

Every verb returns a `ConfigPendingReply`, including refusals and no-ops, which
finish only after return. `options.observe` lists additional exact read handles
to acknowledge after the operation; the initiating handle is always observed
first. The views are validated before anything is admitted: a view that cannot
certify the selected write (an unrelated owner or a narrower selection) refuses
with `read-observation` in phase `selection` and nothing is written.

| # | Verb | Live | Staged | Read-only |
|---|---|---|---|---|
| 1 | `read()` | Ordered barrier capture | Installed staged snapshot, never thawed | Ordered barrier capture |
| 2 | `set(value)` / scope `set([{path, value}])` | Checked intent, read-your-writes | Stages a checked set | `unsupported-operation` |
| 3 | `remove()` = `unset()` | RemoveOverride of every selected declared field | Stages removal; candidate shows inheritance | `unsupported-operation` |
| 4 | `apply()` | Selected persistence (`saveSelected`); runtime root: transient admission, no saved revision | Admits and publishes only the selected staged edits | `unsupported-operation` |
| 5 | `discard()` | `unsupported-operation` (never rollback) | Drops only selected unadmitted edits and rebases them | `unsupported-operation` |
| 6 | `observe(after)` = `whenCurrent(after)` | Capture covering the token; omitted token: fresh barrier | Only an already covered token, else `unsupported-operation` | Capture covering the token |
| 7 | `edit()` | Opens a staged handle over the same selection (store and scope roots) | `unsupported-operation` | `unsupported-operation` |
| 8 | `abandon()` | `unsupported-operation` | Draft sessions: abandons the session's failed admitted Apply (same owner as `draft.abandonApply`); `apply-not-pending` when none remains | `unsupported-operation` |

A batch validates completely before anything is staged or admitted; omitted
fields are not edited; duplicates and paths outside the selection are refused.
A subscope Apply or Discard of a draft leaves disjoint staged edits and their
fences intact. An overlay editor applies or discards its single record slot as a
whole; a subscope that would leave other staged edits of that slot behind is
refused with `selection-partial`. A failed or uncertain admitted Apply keeps its
Retry witness; repeating it follows existing witness recovery.

### The pending reply

| # | Member | Meaning |
|---|---|---|
| 1 | `finished`, `isFinished` | One terminal signal. Connecting right after the call is always safe. |
| 2 | `isError`, `error.code/message/phase` | Value-free error; phase is `selection`, `candidate`, `admission`, `publication`, `observation` or `lifetime`. |
| 3 | `value.generation` | Opaque coverage of this operation; committed write coverage survives editor closure while its owner lives. Staged coverage carries its edit lifetime and cannot stand for committed coverage. |
| 4 | `value.snapshot` | The initiating handle's immutable acknowledged facts. |
| 5 | `value.observations` | One entry per `options.observe` view, in order, with its snapshot and generation. |
| 6 | `value.write`, `value.pending` | Truthful admission/publication facts, and newer local input not covered by the snapshot. |
| 7 | `value.handle` | The handle opened by `edit()`. The edit handle owns its session and its lookups; keep a reference to it while its fields are used, and release it to close the edit. |

Success means the writer finished (admission/publication where applicable) and
then every requested view installed, prepared its bindings, notified and finished
its model updates. A reentrant notification cannot finish it early. Partial or
uncertain publication is an error even if a view observed state. If publication
succeeded but a view's acquisition failed, the reply is an error in phase
`observation` that still carries `value.write` and `value.generation`; retry with
`view.observe(value.generation)`, not another write. Losing the presentation
(closing an editor) never cancels admitted persistence; a retained reply still
receives its completion.

```qml
function applyAutoTile() {
    const reply = edit.apply({observe: [runtime]})
    reply.finished.connect(function () {
        if (reply.isError) { showError(reply.error); return }
        const policy = reply.value.observations[0].snapshot
        autoTile.applyPolicy(policy) // The domain owner consumes the acknowledged value.
    })
}
```

Observation does not certify durability, wait for every process subscriber or
acknowledge an external effect; the application owns its domain actions.

Remaining compatibility routing is explicit: ordinary store/scoped selected and
full-prefix entry points, all specialized retained/profile/migration/collection
producers and every view/model completion path still require the broader routing
and qualification work. The initial slice is not a claim that all old dispatch
paths have already been replaced.

## Selected immutable read facts

Native store, scoped handle and overlay owners expose `readProjection()`.
A draft exposes frozen `readProjection()` and separate live `previewProjection()`.
QML owners expose a stable read-only `readProjection` object. Native
`selectedState(path)` and QML `state(path)` use the owner's existing namespace:
native draft paths remain absolute, scoped handle paths are relative, and overlay
paths are relative to the overlay schema.

| # | Selected fact | Meaning |
|---|---|---|
| 1 | `valid`, `present`, `value` | Selected checked effective presence/value at the installed cut. An invalid state is outside the selection, not a guessed default. |
| 2 | `targetPresent` | Raw presence at the selected writable target before staged edits. It remains distinguishable from inherited effective presence. |
| 3 | `pending` | `none`, `set` or `remove`; ordinary equal sets retain explicit ownership. |
| 4 | `authoredPresent`, `authoredValue` | Checked staged input, distinct from a masked effective result. |
| 5 | `candidateAvailable`, `candidatePresent`, `candidateValue` | Whole-batch checked resolution. Unresolved/failed candidates do not claim guessed inherited values. Getters never acquire or recompute a candidate. |
| 6 | `maskedBy`, `observation` | Mask identity and opaque causal observation from that capture. No raw source tree, rejected data or credentials are exposed. |

Legacy overlay-editor unset still reads absence from `values`. Selected candidate
facts separately show the inherited/effective result. Draft ordinary sets remain
synchronous and introduce no extra public setter completions or busy restriction.
Removal-resolution busy/Apply rules keep their existing meaning. Staging changes
neither real input/intent nor file bytes; Discard drops only unadmitted edits.

## Compatibility synchronization API

```cpp
projection->synchronize(after, context, [](const ConfigReadResult &read) {
    if (read.status() == "observed") {
        const auto admitted = read.snapshot().selectedState({"count"}).value();
        // Give this immutable value to the application's domain owner.
    }
});
```

An empty native `ConfigObservation` requests a new ordered capture barrier.
`ConfigReadResult` contains request ID, `observed` or `failed`, a value-free error,
the installed observation/snapshot, and paths of explicitly newer pending input.
Its `completed` signal precedes the callback. A callback with an independently
supplied context is canceled when that context dies and delivered on its thread.
Frozen editing cannot be silently rebased through synchronization.

The QML compatibility form is:

```qml
editor.apply(function(write) {
    if (write.status === "failed" || write.refreshState !== "current") return;
    runtime.readProjection.synchronize(write.observation, function(read) {
        if (read.status !== "observed") return;
        const policy = read.snapshot;
        // policy.state(["count"]).value is the acknowledged immutable fact.
    });
});
```

`readProjection.synchronize(callback)` omits the token and requests a fresh
barrier. Tokens are library-owned opaque values; a forged JS map is rejected.
Successful writer receipts carry their operation's captured `observation`.
Native `operationCompleted(result, observation)` exposes the captured pair without
changing ConfigResult layout or relying on public signal connection order.
Committed target coverage survives editor closure while its owner lives. Staged
coverage additionally names its edit lifetime/epoch and cannot acknowledge a
committed runtime read.

Composed observations retain two independent kinds of coverage: the actual
atomic owner/base storage cuts, and the selected declared leaves within the
checked record-slot target. Narrowing a snapshot narrows logical coverage, never
its atomic storage dependency paths. A sibling leaf cannot certify another
leaf merely because both use the same record; a reader of the selected superset
can cover it. Candidate tokens keep their preview lifetime/epoch under the same
selection operation. These identities do not create nested persistence routes.

Two acknowledgments are intentional: a saved/current writer does not mean a
different runtime view has installed the result. Equal/no-edit/no-token cases
still acknowledge without fake value notifications. A recoverable read failure
keeps last-good values and subscriptions, returns failure and creates no fresh
success coverage; recovery uses the same retained reader. Actual owner/selection
or preview loss invalidates affected waits. Delayed old captures cannot regress
new state or erase newer local input. Use `read.snapshot`, not a later getter,
when a domain action must consume exactly the acknowledged policy.

Observation does not save, certify durability, wait for every process subscriber,
cancel admitted persistence or acknowledge daemon activation. Partial/uncertain
publication remains failed with its original evidence and Retry facts even if
some state was observed. The application owns external effects.

## Compatibility qualification

The slice preserves released public layouts/metadata slots, old compiled native,
moc and pure-QML clients, `.so.0.5`, the QtCore-only Core boundary and the exact
Qt 6.12.0 private adapter. The export guard remains strict except for reviewed
exact weak compiler-emitted definitions in `tests/generic_primitives/weak_exports.py`.
Each exception requires frozen-baseline `W` linkage and zero undefined imports
from the examined consumers. No pattern/blanket weak exclusion, public-class
vtable/typeinfo exclusion or production ABI shim is used.

`generic_performance` records stage/getter distributions and asserts whole-batch
resolution counts and no candidate reacquisition on control-only publications or
cached getters. Existing timing/sanitizer policy is unchanged. This initial guard
is not a claim of completed full matched-Arch or expanded handle/reply performance
qualification.

## Explicit candidate targets
Effective trees collect winning declared leaves under typed paths and assemble
each namespace once. Atomic Record/Map/List values remain whole leaves; optional
absence, explicit null, exact numeric domains, layer masks and source participation
are unchanged. This avoids repeatedly copying a wide nested namespace for each
leaf in every candidate/publication pass. `compare-performance.py` reports matched
legacy-workflow distributions separately from new handle/reply measurements and
the unchanged caller/model budgets; its marker does not assert domain activation.

Source projection uses that same namespace assembler for each lower-layer cut.
The selected transform still receives the complete preceding layers (not earlier
sources from its own layer), and source admission, rejection rollback and effects
retain their existing ordering. Neither effective reads nor projection inputs
rebuild a shared namespace one leaf at a time.

The shared worker candidate resolver accepts explicit user-layer and runtime-layer
selections. Both enter ConfigLayers' one transactional edit path: all checked
set/remove operations are applied before source projections run once on the whole
candidate. Admission uses the target's existing User/Reset or Runtime/ClearRuntime
input kinds. A refusal discards the candidate without changing the real layers,
intent, files or application effects. Runtime selection never implies persistence.

An empty candidate batch reads the existing admitted cut rather than running a
local edit transaction or rerunning source transforms. Selection/schema/lifetime
checks still apply, and selected facts still pass through current record-list
metadata projection. This does not alter the public layer `applyEdits` contract.

Candidate-only draft control delivery uses the same publication boundary with
the existing admitted store cut. It advances delivery/epoch and installs selected
draft facts without rebuilding unchanged store provenance, values or model plans.
Real store/source/metadata changes still use complete state publication. No-op
control delivery is not a content invalidation or a second state cache.

Draft captures retain their immutable validated P2 batch. Empty initial/Discard
captures and empty live previews share the publication's exact admitted cut;
ordinary value, raw-target-presence, mask and record-list metadata changes reach
that preview without another selection validation or candidate merge. A private
immutable snapshot flag exposes candidate facts lazily from the admitted data,
not by recapturing in a getter. Frozen reads remain frozen; edited candidates
still resolve the complete batch against their explicit frozen/current context.
Empty live previews invalidate only on changes to their selected publication
facts; edited previews retain the source dependency identity. Snapshots capture
immutable facts, never a newer counter substituted for their read cut.

Candidate selected facts are captured directly from that layer cut: declared
defaults, participating admitted contributions, writable-layer masks and the
original target's raw presence. They do not construct full per-source provenance
or rebuild an unselected namespace to obtain a selected value. Full provenance
remains available through `info`; record-list metadata projection still runs on
the selected checked result. No second cache or getter evaluation is introduced.

Source admission does not assemble unused transform inputs. Preceding-layer
leaf facts exist only when source transforms are configured; a consuming layer
assembles its complete preceding namespace once on its first actual transform.
Same-layer sources still see the same preceding cut, and rejection rollback and
effects ordering are unchanged.

Target presence describes the selected target's raw contribution, not the winning
layer or an inferred user route. A runtime candidate does not report its own
runtime contribution as a higher-layer mask. Untouched target contributions and
all other layers remain owned; removing declared leaves does not erase unknown
namespace siblings.

| # | Entry | Shared dispatch |
|---|---|---|
| 1 | Existing `ConfigLayers::applyEdits` | User-target transaction, retaining its existing public signature and projection/rollback behavior |
| 2 | `resolveCandidate`, user or checked record-slot selection | Same user-target transaction over worker-owned candidate layers |
| 3 | `resolveCandidate`, explicit runtime selection | Same transaction with runtime write/erase routing, no user write or disk route |
| 4 | Existing single-value layer/scope writers | Existing checked write/erase owners also used by the transaction; namespace Reset retains its deliberately broad legacy meaning |

This target adapter is a Phase 2 increment, not the generated-handle/reply surface
or completion of all facade routing. New handle construction will select runtime
explicitly; the compatibility APIs have not changed.

## Pending completion owner (Phase 2 increment)

`ConfigPendingReply` is the native terminal envelope for the forthcoming handle
verbs. `ConfigQmlPendingReply` exposes `finished`, `isFinished`, `isError`, `error`
and lazy `value`. This increment supplies the types and private read-composition
entry point, not the generated handle factories or public handle verbs yet.

The private composition validates and captures the exact additional reader list
before dispatch. It obtains the initiating cut through the existing P3 projection,
then asks each captured reader to cover that cut. A superset reader is permitted;
an unrelated or narrower reader refuses before capture. No new merge, read cache,
worker stream or completion counter is introduced. Every refusal, no-op and success
finishes queued after return. Terminal delivery is once only, after each successful
reader's installation/notification completion, independently of public signal order.

| # | Captured reply fact | Meaning |
|---|---|---|
| 1 | `generation`, `snapshot`, `pending` | Detached initiating coverage, selected immutable facts and newer input at its acknowledgment; not later getters |
| 2 | `observations` | Additional readers in invocation order, each with its own snapshot, generation and pending facts; captures need not share one global cut |
| 3 | `write` | Absent for reads; observation alone never creates a publication receipt |
| 4 | `error.code/message/phase` | Value-free failure, including selection, acquisition and lifetime failures |

If an additional read fails, the initiating successful cut is retained. Failed
read entries carry their last-good state, not fresh success coverage. Entries not
acknowledged before failure have status `not_observed`; the whole reply is an
error, never partial observation success. Actual source-presentation loss fails
pending delivery; detached results remain readable afterward. The eventual writer
adapter must additionally retain its paired publication facts on observation
failure; this increment does not yet compose writer completion or Retry.

Native replies have explicit caller ownership. The QML wrapper owns its native
reply without parenting itself to a store/editor. Retained replies survive source
presentation closure; unreferenced pending or settled wrappers and their native
payloads are engine-GC-reclaimable. Callbacks retain only guarded native pointers,
not engine values or a store-owned reply registry. Schema handles, edit opening,
selected writer verbs, frozen-handle reads and their full compatibility matrix
remain subsequent Phase 2/3 work.

## Generated-handle selection foundation (Phase 2 increment)

The private `ConfigHandleSelection` describes a generated handle's schema
namespace and narrowed P1 selection. Group/leaf lookup and disjoint selections
reuse the existing validator and retain schema identity, target, owner and lifetime.
Opaque path segments, optional defaultless fields and names such as `read` or
`__proto__` are declarations, never verb dispatch or inferred persisted fields.
Records/Maps/Lists remain atomic leaves, including their declared descriptors.

Child lookup and group expansion are restricted to the parent's allowed leaves;
neither can widen a read-only target or a selected union. Invalid/empty/duplicate
selections stay invalid rather than falling back to root. The selected descriptor
contains only allowed declared children and is detached from caller mutation.
This foundation performs no IO, value resolution, worker capture or input.

It is not the public generated QObject graph yet. Lazy handle materialization,
new native/QML root accessors, all reply-returning verbs and selected edit-session
Apply/discard remain required; those adapters will use this identity instead of
adding a parallel selection validator or configuration cache.

## Selected read projections (Phase 2 increment)

A selected projection narrows an existing store, scoped, draft, preview or
overlay projection to a generated-handle identity. It is created privately from
that parent projection and the identity above; it never widens the parent's
leaves, namespace prefix, schema identity, target or lifetime.

| # | Behavior | Contract |
|---|---|---|
| 1 | Capture | Only the selected sources are preflighted. A corrupt disjoint source does not fail a selected leaf read, while a root read still fails. |
| 2 | Composed overlays | Owner/base capture and atomic cuts stay complete. Each selected receiver narrows facts and logical coverage, so a sibling's token does not cover it. Request IDs are projection-local and correlate only with the initiating projection. |
| 3 | Candidates | Draft and overlay candidates and pending edits come from the same worker candidate as the parent; there is no second evaluator. Frozen selected reads refuse with `read-frozen`; Discard returns them to the committed state. |
| 4 | Lifetime | Parent detach, destruction or preview closure invalidates the selected projection and fails its pending reads. Retained snapshots stay readable. |

This is still private plumbing for the generated handle graph. Public handle
objects, root accessors and handle verbs remain required subsequent work.
