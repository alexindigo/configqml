# Declared two-owner overlays

`openOverlay` combines a declared base group with one record's instance override.
The worker owns selection, merge, sparse difference, raw placement mapping and
revision fences. The GUI receives a schema-backed namespace, never a raw mirror.
Core remains QtCore-only; there are no merge/edit scripts or application-specific
widget identifiers in the library.

```qml
main.openOverlay({
    mode: "editor",
    base: shared.scope(["settings"], false),
    ownerPath: ["rows"],
    ownerGeneration: main.info(["rows"]).publicationGeneration,
    entryPath: [{index: 0}],
    identityPath: [{key: "id"}],
    identityValue: "application-authored-id",
    overrideKey: "instanceSettings",
    mergePolicy: "recursive_objects_replace_lists",
    differencePolicy: "top_level_omit_equal"
}, function(result) {
    if (result.status === "opened") {
        const editor = result.editor;
        editor.values.volume = 0.5;
        editor.apply(function(receipt) {
            // A successful save with stale refresh still needs resolution.
            if (receipt.status !== "failed" && receipt.refreshState === "current")
                consumeConfirmedEditor(editor);
        });
    }
});
```

The options are exactly the ten shown members; only `mode` is optional, default
`"editor"`. Mode may also be `"view"`. The base must be a living scoped handle
over a declared group. `ownerPath` selects an atomic List/Record/Map leaf and
must be writable in editor mode. `entryPath` selects a record inside that leaf;
`identityPath` is nonempty and relative to the record, selecting a typed scalar.
`overrideKey` is one nonempty opaque field name, including a permitted unknown
record member. Unknown options or policies fail `overlay-options`; invalid
scope/schema selection fails `overlay-selection`, mismatched identity
`overlay-identity`. Conversion errors throw synchronously with `inputRejected`;
semantic refusals complete asynchronously with a normal request ID/result.

Value paths are arrays of exactly `{key: string}` or `{index: integer}` segments.
Indices are nonnegative safe integers; policy-only `{each:true}` is not an edit
address. Keys are opaque, with no dot/slash interpretation. Nested screen-like
and desktop-like records use the same addressing. Repeated IDs are allowed:
identity checking does not invent unique persistent placement IDs.

## Values and sparse persistence

`recursive_objects_replace_lists` recursively merges only when both values are
nonnull dictionaries. Otherwise the override replaces the base, including lists
as whole values. Missing override means `{}`; present null/nonobject override is
an error. Optional absent leaves stay absent. Null, false, zero, empty string,
empty list and empty dictionary retain their normal descriptor semantics.

`top_level_omit_equal` compares each present edited top-level key to the captured
base using schema-domain equality. Equal keys are omitted; a different key keeps
its whole edited value, not a recursively sparse difference. An absent edited key
does not create a deletion tombstone for shared settings. An empty difference
removes the override field. Unknown existing top-level overrides remain owned on
the worker and survive known-field edits without dynamic namespace properties;
unknown members within atomic known values follow normal schema preservation.
Explicit `resetOverrides` removes the entire override field, including opaque
siblings. Neither command saves the base store.

An editor's `values` are detached from the main store until Apply. Checked equal
assignments are dirty intent. `unset(relativeLeafPath)` removes one declared
preview leaf; it returns a Boolean ingress result, not a save receipt. Namespace
groups cannot be assigned or unset. Int64/UInt64 keep the usual decimal-string
QML representation; native values retain their exact signed/unsigned domains.
For an opaque key colliding with JavaScript's prototype accessor, read the own
data property explicitly, e.g. `Object.getOwnPropertyDescriptor(values.keyMap,
"__proto__").value`. Qt's attached QVariantMap reference preserves that own key
but ordinary `.__proto__` lookup accesses the wrapper's JavaScript prototype.

## Live view versus frozen editor

| # | Surface | Behavior |
|---|---|---|
| 1 | `mode: "view"` | Read-only live namespace. Worker subscriptions refresh the current checked displayed address after base/owner changes. Missing/replaced placement gives Error/`overlay-identity`; base scope/owner closure gives Error/`overlay-owner`. Writes/unset/apply/reset/discard refuse `overlay-readonly`. |
| 2 | `mode: "editor"` | Frozen base and unnormalized owning-leaf capture. Equal intent, ABA, raw invalid/masked input, inode/provenance change, reorder or list-policy remapping invalidates its fences. Dirty preview is retained on conflict. |

Open settles/preflights selected owner/base paths on their workers. The displayed
generation and normalization maps resolve placement back to the unnormalized
owning leaf. Housekeeping-injected defaults and removed fields are not serialized
as edits; untouched hidden rows, deprecated fields and opaque data remain intact.
Ordinary whole-leaf setters still mean a complete authored choice.

Apply obtains short selected read leases in stable endpoint order, validates
content, intent and ingress fences, and admits one main owning-leaf replacement
through common checked admission/intent recording. Self/cyclic dependence fails
`overlay-cycle`. Selected mutations queue; disjoint worker paths can progress.
Leases release by ordered messages after admission, before publication IO. No
blocking cross-worker wait, nested event loop or handshake timeout is introduced.
Close, detach, scope destruction and worker failure explicitly cancel the leases.
This is not external-writer CAS or a two-file transaction; normal source preflight,
strict-preimage, reservations, masks and publication evidence still apply.

## Handles, receipts and lifetime

Handles expose read-only `editorId`, `values`, `ready`, `dirty`, `conflicted`,
`busy`, `status` (`Loading | Ready | Error | Closed`), `error`, `stateChanged` and
`completed(result)`. Operations return request IDs immediately and complete in the
main store's existing order, after eligible cache installation, signal before
detached callback. While busy, edits/apply/reset/discard refuse `overlay-busy`.

`apply` and `resetOverrides` return exactly `saved | nothing_to_save | failed`,
the normal route/mask/coverage/evidence fields, `editorId`, and `refreshState`:

| # | Refresh state | Meaning |
|---|---|---|
| 1 | `current` | Confirmed main state and compatible base recaptured; dirty/conflicted clear. |
| 2 | `stale` | Publication still has its truthful save status, but later owner/base ingress prevents refresh. Preview remains dirty/conflicted and must not be silently closed or discarded. |
| 3 | `not_refreshed` | Failed admission/publication; preview and failure evidence remain. |

`discard` asynchronously recaptures current base/placement, returning
`discarded | failed`; only success drops the preview/conflict. The application
obtains user confirmation before discarding work. `detach` closes delivery and
reclaims captures/subscriptions; already admitted requested persistence survives
editor/main-consumer destruction. Unapplied manual edits gain no implicit save.
Destroying a callback context suppresses its delivery. A closed scope/owner never
retargets the handle to another store or placement.

Native callers include `configoverlayeditor.h`, populate `ConfigOverlayOptions`
with the same field names and typed mode/merge/difference enums, and call
`ConfigStore::openOverlay(options, context, callback)`. Resolve the returned handle
through `overlayOpened(requestId, ConfigOverlayEditor*)`, emitted after handle
installation and before the normal `completed(opened)` result; native results
contain `editorId`, not a QObject payload. `value`, `setValue` and `unset` use
relative ConfigPaths, and Apply/reset/discard use normal native callbacks.
