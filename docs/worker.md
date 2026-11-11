# Native ordered owner

`configqml::core` is C++17/QtCore-only. `ConfigStoreOptions` supplies an owned
schema, explicitly ordered raw layers and optional worker-safe native admission
predicate. Construct and call `ConfigStore` on its object thread. Each store
has one ordered worker that owns ConfigLayers, merge/diff/admission, dirty
intent, the memory backend, publication/readback and persistence decisions.

## Input and authoritative settlement

`setUser` performs bounded schema/ownership and immutable write-capability checks,
owns the path/value, allocates its sequence, registers the callback and queues
the command before local exposure or notifications. Equal assignments retain
intent. Unmasked `value(path)` reads accepted local input immediately; masked
reads retain the higher-layer effective value and report its identity. A path
without a writable user route fails synchronously without an input event or
local replacement. Runtime commands are explicitly separate from persistent
user assignment. Reset checks the named writable target and subtree routing.

Every settled input passes through common sequence/path-generation coverage,
including refusal after terminal worker failure. Eligible authoritative values
are installed before any failure delivery. An older failure cannot replace a
newer pending local write; settlement of that latest write restores the last
authoritative value. Failed intent/evidence is retained separately from getters.
Topology, input, path-generation and publication-generation fences stay active.

Per-path `publicationGeneration` fences selection freshness, not delivery of
provenance metadata. Confirmed own save/readback keeps that fence when displayed
and raw content, intent/ingress and list metadata/mapping are unchanged. Exact
before/after contribution transitions establish own confirmation; a receipt alone
does not. Native numeric storage becoming its identical JSON token is not new
content, without rounding or rewriting exact atoms. Confirmation still delivers
fresh provenance in independently ordered snapshot packets; delayed equal-fence
metadata cannot roll it back. Foreign observations, other-writer intent, hidden
raw changes, remapping and metadata updates retain their stale-selection fences,
including foreign preflight before an otherwise successful own save.

Grouped publications install all eligible caches before notification. One shared
`ConfigConsumerGuard` governs synchronous consumer emissions/callbacks: it holds
QPointer lifetime checks on the stack and callers stop continuation if an emitter
or dependency was destroyed. Callback state is detached before reentry. Signals
precede callbacks; request completion order is preserved. A consumer destroying
the store during notification cancels further store-owned delivery. Queued native
callbacks guard their independently supplied living context.

Packets distinguish complete state from absent state. Retained-group info and
lifecycle generations travel together in an optional, publication-stamped
snapshot: absent means unchanged; present and empty is authoritative. The facade
installs a newer complete group view before any notification, independently of
pending ingress claims. Diagnostic, warning, effects, metadata, readiness and
other partial packets cannot clear it; older full snapshots cannot roll it back.
Held replies retain capture-time state rather than receiving a newer worker view
when delivered. Source states remain optional snapshots; values/models/overlays
are keyed deltas, and completion/status/diagnostic fields are explicit events.

## Requests, snapshots and lifetime

`save`, `flush`, `close` capture the reported input prefix, never a facade graph.
Save results are exactly `saved`, `nothing_to_save`, or `failed`, asynchronously.
They name request/store/prefix, confirmed revisions, per-route publication facts,
known/uncertain/partial state and value-free error codes. Ordinary publication
failures remain attached to pending source work, not every future save: unrelated
routes still publish, and later saves, including autosave, can settle a resolved source
against its original preimage. Old receipts remain in `failures()` as evidence.
Rejected inputs have their own failed receipt and remain in `failures()`, but
cannot make a later publishing save fail. Terminal worker loss still stops
persistence and propagates its uncertainty to queued requests. Memory
publication confirms readback bytes; it is not disk durability.

Flush requests publication and waits for earlier work. Close seals ingress,
flushes, settles and releases the worker. Read-only stores do not publish on
flush/close. Unexpected worker failure stops persistence and still settles queued
inputs authoritatively. There is no hidden owner recreation or replay.
Close completion follows actual worker-thread teardown (including thread-local
destructors), with nonblocking join polling by an independently retained lifetime
owner. Destroying the store drops consumer delivery, not accepted persistence:
autoSave or an outstanding save/flush/close hands its reported prefix to the
detached worker for settlement. There is no destructor event drain or cancellation.
Without autoSave or an outstanding persistence request, manual unsaved edits do
not gain implicit saving. Destroyed callback contexts cancel delivery.

A process-wide registry retains live worker owners, dispatching only owned
commands and tracking their prefixes/requests. AboutToQuit and qAddPostRoutine
drain all owners within one shared five-second budget, independent of any store
QObject. Post-routine cleanup covers returning from main without exec/quit;
unsettled work and detached failure are reported on stderr. This registry owns
lifetimes, never raw merging or shared store values. See [json-files.md](json-files.md)
for policy, timeout and receipt details.

`snapshot()` returns the most recent immutable worker-admitted generation and
input prefix; `value()` is the separately fenced local read-your-writes getter.
Snapshot construction and replaced-snapshot reclamation remain worker-owned.
`info()` distinguishes latest local input from worker-covered state. Explicit
`inputs()`/`failures()` return evidence copies, not save captures. Native clients
may transfer immutable snapshots across threads; live store access stays on its
object thread.

`SafeInteger` admits only signed integral values in ±(2^53−1), consistently in
native/raw/environment paths. Existing Integer/UnsignedInteger descriptors keep
their exact qint64/quint64 domains. Missing, nullable, finite Real and URL rules
remain the native schema contract.

Uncertain publication witnesses share one worker-owned provenance registry with
watch/reload admission and Retry, for ordinary and retained work. A successful
read of the exact witnessed revision AND bytes is retained as `own` under the
original request/group/attempt correlation, not admitted as a new external disk
choice. Watches remain active and failed work stays pending/stopped. Another
revision, even with identical bytes, remains external. Mixed-source reads admit
foreign sources without exposing uncertain own output as an external fact.
Retry validates the causal capture, then executes normal publication/readback;
only successful confirmation advances waiting own descendants and covers intent.

## Verification

Native CTest covers ordered exact-prefix persistence, held effects/replies,
ordinary and terminal rejection, masks, read-only capability, partial/uncertain
outcomes, context cancellation, emitter destruction at each native boundary,
thread affinity, timings and Core-only linkage. Builds and tests use the dedicated
feature VM named by [RELEASING.md](../RELEASING.md), with disk-backed TMPDIR.
Run the complete suite in Debug with `QT_FATAL_WARNINGS=1`, and the sanitizer
build too.
