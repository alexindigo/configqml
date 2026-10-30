# Retained groups and explicit retry

Retained groups are store-owned captures on the same ordered worker as ordinary
writes. They do not recreate a draft from the current facade on Retry.

```qml
store.retainGroup({
    requestKey: "application-operation-id",
    paths: [["bindings"]],
    edits: [{path: ["bindings", "environment"], present: true, value: "macos"}],
    dependencies: []
}, function(result) {
    if (result.status === "retained")
        store.saveGroup(result.groupToken, applicationReceipt);
});
```

`retainGroup` returns a request ID immediately and later `retained | failed`.
Selections are declared leaves/namespaces, edits must be disjoint and inside
writable selections, and reset is `present:false` with no value. All edits admit
or none do. The worker captures selected pending intent, the original command,
source preimages, input/intent coverage and earlier dependency tokens before
publishing the preview. A stable requestKey with the same checked body returns
the original token/capture; a different body fails `group-key-mismatch`.
Identical requests are lookups, including while the first request is pending and
after later intent or release. They allocate a new ordered request/callback only:
no new input prefix, input record, path/intent/content generation or endpoint fence.
Reservation-only requests likewise capture existing intent without creating a
settings mutation. Retry creates a new publication attempt, not new settings
intent; neither Retry nor a refused repeated attempt recaptures the current display.

`groupInfo(token)` is a cached, value-free record containing groupToken,
requestKey, state, capturePrefix, coverage, dependencies and attempts. Unknown or
other-store tokens return `group-token`. Native counters are quint64; QML IDs,
prefixes and attempt numbers are strings. No bytes, command body or raw settings
are returned by groupInfo. Cache installation precedes completed and its detached
callback, using the normal consumer guard and request ordering.

`saveGroup` accepts retained state; `retryGroup` accepts stopped state. Every
attempt has a fresh request/attempt ID and increments attemptNumber. Both produce
the ordinary `saved | nothing_to_save | failed` persistence receipt with the
original capture fields, attempt identity, unresolved/excluded tokens, confirmed
revisions and actual per-route outcomes. Busy, succeeded and released attempts
refuse with `group-state`. Old failed attempts remain available after success.

Admission is tied to a **lifecycle generation captured at submission**, not just
the last displayed state string. The object thread orders pending saving/release
claims before returning a request ID. The worker advances its generation when an
attempt starts and when it settles (including failure), and on release. Older
publications cannot roll back a pending ingress claim. A Retry submitted while an
earlier attempt is saving therefore fails `group-state` even if that attempt later
returns to stopped; a release submitted while saving cannot become valid after
success. Repeated requests in one turn obey the same fence.

Retained lookups, selected overlapping reservations and declared dependencies use
the same captured generations. An intervening lifecycle change makes that request
stale (`group-state`), rather than silently using the eventual state. Disjoint
groups are not invalidated by unrelated lifecycle changes. Once the prior request
settles, a fresh explicit request captures the new generation and may proceed.
`groupInfo` remains a worker-published display cache, not admission authority.
These internal generations are distinct from attempt numbers and settings mutation
input/path/endpoint fences; capturing or refusing a lifecycle request creates no
new settings intent and never recaptures or discards the retained body.

## Reservations and causal order

All persistent producers feed one worker intent ledger. Ordinary/autosaves build
candidates from confirmed raw plus eligible ordered intent, excluding reserved
paths even if the facade displays newer pending choices there. Disjoint values
in the same file remain saveable; unknown raw data is preserved. A queued earlier
save cannot include a later capture. The GUI is not a candidate/raw owner.

**Overlapping unresolved captures must declare an earlier dependency**, directly or
transitively; omission fails `group-dependency`. Disjoint captures need no edge.
A stopped dependency causes `group-dependency-stopped` without filesystem effects.
It leaves that suffix stopped with its original capture, and head Retry never
implicitly retries suffixes. Applications explicitly retry their FIFO tokens.

Successful own publications advance causal source witnesses. Foreign reads never
replace a capture's preimage. Partial attempts remember confirmed routes, so Retry
does not republish them. An uncertain own effect retains its correlated publication
identity and exact candidate bytes; explicit recovery reads must match both before
a new attempt can publish. Foreign or unfamiliar output fails rather than rebasing.
Recovery reads use asynchronous backend scheduling; there is no GUI lock or nested
worker wait. A publication witness is not a successful durability receipt.

Namespace resets retain their original namespace conflict fence while fragmenting
their pending effects along declared leaves/unknown members. This lets a selected
leaf remain reserved while unrelated reset work persists. Conditional cleanup
removes only empty declared namespace scaffolding; it cannot remove explicit
atomic empty values. Shared cleanup completes when the last reserved fragment is
published, preserving both reservation isolation and namespace-reset semantics.

Manual full save/flush/close fails `unresolved-group` while covered retained/stopped
work remains, even when eligible routes published. Such refusal does not poison
future saves after explicit resolution. Autosave reports its eligible subset and
excluded groups; it never claims full-prefix durability for excluded work.
Group failures remain separate from the failure history of unrelated ordinary saves.

Success resolves that capture's reservation; newer accepted local intent remains
pending independently. `releaseGroup` is asynchronous `released | failed`, allowed
after success only. It releases captured source data and retains a value-free
tombstone/history sufficient for refusal and request-key idempotency. It never
discards unsaved work. Destroying a popup/context cancels delivery, not the
store-owned capture or an already requested publication. Store/process shutdown
continues to obey the existing explicit lifetime and bounded shutdown contract.
