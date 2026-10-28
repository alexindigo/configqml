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

Grouped publications install all eligible caches before notification. One shared
`ConfigConsumerGuard` governs synchronous consumer emissions/callbacks: it holds
QPointer lifetime checks on the stack and callers stop continuation if an emitter
or dependency was destroyed. Callback state is detached before reentry. Signals
precede callbacks; request completion order is preserved. A consumer destroying
the store during notification cancels further store-owned delivery. Queued native
callbacks guard their independently supplied living context.

## Requests, snapshots and lifetime

`save`, `flush`, `close` capture the reported input prefix, never a facade graph.
Save results are exactly `saved`, `nothing_to_save`, or `failed`, asynchronously.
They name request/store/prefix, confirmed revisions, per-route publication facts,
known/uncertain/partial state and value-free error codes. Failure propagation
preserves original outcome evidence without silent retry or recapture. Memory
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

## Verification

Native CTest covers ordered exact-prefix persistence, held effects/replies,
ordinary and terminal rejection, masks, read-only capability, partial/uncertain
outcomes, context cancellation, emitter destruction at each native boundary,
thread affinity, timings and Core-only linkage. Builds and tests run in agora
VM 806 `dev-niri-configqml` via `agora-niri`, with disk-backed TMPDIR. Run the
complete suite in Debug with `QT_FATAL_WARNINGS=1`, and the sanitizer build too.
