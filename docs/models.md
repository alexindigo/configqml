# Read-only collection projections

`ConfigStore::listModel(path)` and `keyedModel(path)` return cached, store-owned
`ConfigListModel` handles on the store's object thread. The QML store exposes the
same methods. Paths are arrays of opaque segments. An invalid kind/path returns
null natively and throws a value-free diagnostic in QML. A valid native handle
can be obtained while Loading and receives its initial rows before Ready.

```qml
readonly property var cfg: store.values
readonly property var widgets: store.ready ? store.listModel(["bar", "widgets"]) : null
ListView {
    model: widgets
    delegate: Text {
        required property string rowId
        required property string title
        text: title
    }
}
```

## Identity and notifications

Set `ConfigValue.idField` (native `ConfigDescriptor::idField`) on a list of records
to select its identity field. That field must be required, nonnullable, nonforbidden
and String, Integer, Int64 or UInt64 (native String, SafeInteger, Integer or
UnsignedInteger). Admission in every layer checks uniqueness, including defaults
and nested collections. Duplicate IDs reject the value with `duplicate-row-id`;
there is no ambiguous best-effort matching.

```qml
ConfigValue {
    key: "widgets"; valueType: ConfigValue.List; idField: "key"
    defaultValue: [{key: "clock", title: "Clock"}]
    ConfigValue {
        valueType: ConfigValue.Record
        ConfigValue { key: "key"; valueType: ConfigValue.String }
        ConfigValue { key: "title"; valueType: ConfigValue.String }
    }
}
```

Without idField, identity is explicitly **index-only**. Reordering values updates
the same positional rows; focus/selection does not follow those values. With
idField, small incremental moves preserve persistent indexes, live delegates and
ListView's current item for surviving keys. Changing an ID means removal/insertion.
Removing the selected key leaves selection fallback to the view. Large changes
use one synchronous modelReset, as specified below; consumers recover selection
with `model.indexOfRow(savedRowId)` instead of retaining indexes/delegate objects.

Maps use the exact opaque map key as rowId and deterministic QString lexical
ordering. Keys containing dots, slashes, spaces, `..`, underscores or Unicode are
never parsed into paths or filenames. These are configured keyed preferences,
not an inventory of currently detected devices.

| # | Role | Value |
|---|---|---|
| 1 | `modelData` (Qt::UserRole) | Primitive value, or immutable ConfigNode for a composite row. |
| 2 | `rowId` | Declared key, positional integer, or opaque map key. |
| 3 | Record field names | Scalar field value, or immutable ConfigNode for a nested field. Absent optional fields return an invalid value. |

`modelData` and `rowId` are reserved record field names at collection element
boundaries. The schema rejects collisions. Qt's standard role/selection names
should also be avoided by consumers when choosing field names. Native models
preserve exact qint64/quint64 roles; QML-constructed stores select the Core model's
`Numbers::DecimalStrings` presentation for these domains, including rowId and
nested nodes. SafeInteger stays a number. This option affects presentation only,
not admission, property getters, snapshots, layer storage or JSON encoding.

## Protected nodes and editing

ConfigNode is an immutable, value-owned Q_GADGET, registered as a QML value type.
Native callers construct snapshots with `ConfigNode::fromValue(schema, path, value)`;
the factory uses the immutable owned schema and the canonical deep-owning admission
boundary. Invalid input produces an invalid node. Raw trusted construction is
private to model producers and node traversal, so role reads do not deep-copy
already-owned subtrees.
It exposes read-only `value`, `keys`, `count`, `valid`, plus `get(opaqueKey)` and
`at(index)`. Composite `value` is invalid: traverse nodes instead of obtaining a
mutable backing map/list. Missing children and invalid indexes return an invalid
node. Finite mixed record/map/list trees follow the existing schema depth limit.

```qml
const oldRow = delegate.rowSnapshot
const label = oldRow.get("children").at(0).get("label").value
```

Retaining a node retains that snapshot, even after rows change, disappear, or the
store is destroyed. Nodes do not become live writable aliases. Their Qt value
storage is released when the last copy is released; the model does not retain
every historical row QObject. Scalars/keys are detached values. Assignments to
node properties fail visibly in QML. Editing a detached scalar/value cannot edit
the store.

Models have no membership-editing API. Inherited Qt insert/remove/move methods
refuse changes; `setData` always returns false and emits writeRejected. The QML
store translates role-write rejection into `read-only-model` and inputRejected.
Delegates send injected editing intents to the owner, which assigns the complete
list/map setting or uses a store command. Explicit equal assignments still enter
ordinary ingress; model publication never creates new input.

For keyed lists, build a detached replacement before swapping IDs or reversing:

```qml
const edited = cfg.bar.widgets.slice()
edited.reverse()
cfg.bar.widgets = edited
```

A property-attached reverse/sort can invoke several setters (M0 contract), and an
intermediate duplicate ID is invalid. Such operations are not hidden transactions.

## Worker ownership, fences and lifetime

Models project eligible **worker publications**. Immediate unmasked property
read-your-writes remains local ingress state; a held worker can leave a model at
the prior publication while the property getter shows newer pending input.
`info(path).workerCovered` distinguishes that state. A stale reply cannot replace
either the newer local property value or its model. On eligible publication,
the store installs all eligible property caches first, applies legal model
notifications synchronously on the model thread, then emits property/status
notifications and ordered completions. Completion observers see the settled
property/model generation. Standard aboutTo/end model signals necessarily expose
the legal intermediate row structure inside a multi-edit batch.

The worker owns row extraction, hash-keyed identity matching, longest-increasing-
subsequence (LIS) move selection and precise role-change calculation. Order-statistic
treap ranks/relocations keep small-plan index calculation O(N log N), without
repeated list searches or renumbering. The object thread selects a precomputed
plan by its displayed base; it never diffs or synchronously queries the worker.
The worker retains unacknowledged bases and computes a plan for each,
then reclaims older bases after an asynchronous acknowledgement. Skipping a stale
reply therefore does not replay transient removals or destroy a key that survives
in the final eligible publication. Outstanding input/reply backlog determines the
number of temporarily retained bases; ordinary acknowledged operation retains
only the current base. Reconciliation is O(N log N) per retained base (plus value
comparison proportional to the data); backlog-dependent work remains explicit.

### Large plans and selection recovery (decision 12)

If the calculated notification count (inserts + removes + LIS moves + changed
rows) exceeds **max(64, previous rowCount / 4)**, the worker chooses one reset.
It supplies the final owned rows and a row-ID index hash. The object thread swaps
that prepared payload inside a single legal beginResetModel/endResetModel pair.
This also bounds initial large insertion, mass removal and mass nested-field
replacement. No partial publication, event-loop slicing or hidden queue of model
chunks is introduced. Small plans retain precise notifications and identity.

After reset, `indexOfRow(rowId)` returns the new row index, or -1 if absent.
For a settled generation this uses the worker-prepared hash in O(1) expected time;
inside a small plan's intermediate aboutTo/end notifications it searches the
currently legal intermediate row structure. Keep the selected key in the consumer
and restore currentIndex after reset. Persistent indexes/delegate QObject identities
are not preserved by a reset. A full reversal has N-1 LIS moves and intentionally
uses this fallback at 1,000 rows.

### Presentations and admission

Native and dynamic QML facades share `ConfigStore::presentation(path)`. It caches
the pure, detached schema-directed QVariant presentation for each visible setting
value, invalidated only when an eligible publication or local ingress changes that
value. The first read builds it once; subsequent getters return shared value
storage without walking or converting the list/map again. Stale publications and
equal/masked writes cannot invalidate the visible presentation incorrectly.

Core schema validation owns, normalizes and validates in one schema-directed
traversal. The QML adapter exposes a synchronous input view, not an intermediate
QVariantList/QVariantMap tree for Core to recapture. Only the final admitted
containers are built. Known field keys share immutable owned schema storage;
unknown/map keys and string values are captured. Wide decimal strings are parsed
directly into exact integer primitives. Cycles/engine-bound values remain rejected.

`ConfigSchema::admit` produces a sealed `ConfigAdmission` containing owned data
and path, tied to that immutable schema. Core setters accept it without a second
capture/validation walk, while still checking store status, write capability,
masking and sequence/worker admission. Default/foreign-schema or inconsistent
input-view tokens cannot bypass ingress. This token means synchronous schema
acceptance, not worker admission or persistence. Native QVariant setters, raw
sources and declarations use the same capture/validation rules. Trusted node
traversal does no ownership recapture. See [ownership.md](ownership.md).

Model objects are held independently of the consumer QObject during each legal
begin/end pair. Synchronously destroying a store in an aboutTo handler cannot
free Qt's still-active model private state. After that pair, the owner guard
stops further edits/delivery. Consumers borrow model pointers and must not delete
them directly. Nested event processing cannot interleave another model batch;
new packets queue until the active batch ends. Reentrant writes still enter the
next owned, sequence-fenced input prefix normally.

## Verification

Run the full Debug and ASan/UBSan CTest suites in agora VM 806
`dev-niri-configqml`, workspace `/home/tester/Projects/cqm4`, via agora-niri with
timeouts and disk-backed TMPDIR. `models` uses QAbstractItemModelTester and native
consumers; `qml_models` cold-loads real offscreen ListView/Repeater delegates.
`models_qml_lint` checks the import graph. Test-only Gui/Quick dependencies do not
enter `configqml::core` or `configqml::configqml`; `core_link` checks production
closure. The 1,000-row small-rotation test preserves delegates without reset; the
full reversal asserts a single reset and key lookup recovery. Release regressions
enforce <16ms library ingress + publication work at 1,000 rows, <50ms worker
planning at 1,000 and <500ms at 10,000, for lists and maps through writes, reorder,
insertion and nested changes. Cached getters and nested-node traversal are also
budgeted. Debug/sanitizer lanes retain every functional assertion but do not
assert wall-clock timings.

Library publication timing includes cache/fence/plan/index updates and excludes
time inside synchronous Qt notifications (including Qt view callbacks); that
notification/view cost is reported separately. The large-change path emits the
same single Qt reset primitive as a plain-reset timing control, with no extra
per-row notifications or delegate-model layout tricks. Timing output reports both
controls; view/delegate construction and application code are not mislabeled as
library-only work or a full-shell 16ms guarantee.
