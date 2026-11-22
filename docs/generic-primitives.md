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

This slice provides the compatibility `readProjection` API described below. The
schema-generated handle graph, uniform verbs and composed pending replies are
the follow-on public interface; they adapt these same contracts, not another
capture owner. They are not implemented by the initial slice. Existing methods,
values, numeric domains, namespaces and persistence/Retry behavior remain valid.

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

The shared worker candidate resolver accepts explicit user-layer and runtime-layer
selections. Both enter ConfigLayers' one transactional edit path: all checked
set/remove operations are applied before source projections run once on the whole
candidate. Admission uses the target's existing User/Reset or Runtime/ClearRuntime
input kinds. A refusal discards the candidate without changing the real layers,
intent, files or application effects. Runtime selection never implies persistence.

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
