# JSON files and save policy

The native Core-only worker supports memory and JSON sources in the same ordered
layer topology. Files are explicitly named: no directory enumeration, filename
inference, environment search ladder or effective-value serialization.

## Native declarations

```cpp
ConfigStoreOptions options;
options.schema = schema;
options.layers = {{"user", ConfigLayerKind::File,
    ConfigAdmissionPolicy::FieldFallback, {{"bar", {"bar"}, {}}, {"ui", {"ui"}, {}}}, true}};
options.jsonFiles = {{"user", "bar", "/home/me/.config/example/bar.json", true, ConfigWatchMode::Live},
                     {"user", "ui", "/home/me/.config/example/ui.json"}};
options.autoSave = true;           // default
options.autoSaveInterval = 300;    // milliseconds; default
ConfigStore store(options);
```

Each JSON descriptor references exactly one File-kind layer/source. JSON source
IDs must be unique across the store; paths must be distinct, absolute and clean.
Existing layer rules reject overlapping shard prefixes and more than one writable
layer. Multiple read-only file layers precede that layer in declared order.
Source raw maps are relative to their namespace prefixes. An empty path selects
`QStandardPaths::AppConfigLocation/config.json`; set the application identity
before construction. Explicit replacement paths are recommended for tests and
applications with an existing file layout. Library defaults stay in the schema;
shipped/system files enter only through explicitly declared read-only layers.

```qml
ConfigStore {
    schema: applicationSchema
    autoSave: true
    autoSaveInterval: 300
    sources: [JsonFileSource {
        layerId: "user"; sourceId: "bar"; prefix: ["bar"]
        path: applicationConfigDirectory + "/bar.json"
        writable: true; optional: true
        watchMode: JsonFileSource.Live
        admissionPolicy: JsonFileSource.FieldFallback
    }]
    onAutoSaved: function(result) { applicationSaveResult(result) }
}
```

One declaration per shard, shared layerId and nonoverlapping prefixes. Omit prefix
for a single root file. `JsonFileSource` shares the existing source declaration
compiler; inherited `data` must be empty and kind must be File. Schema, sources
and save-policy declarations compile once at construction. Runtime declaration
mutation does not reconfigure an existing store.

## Raw data and admission

`ConfigJsonCodec` retains exact number lexemes, unknown keys and original object
key order for surviving keys (new keys append deterministically). Arrays remain
atomic settings. Whitespace and comments are not preserved; JSON comments and
duplicate keys are rejected. Files must contain an object, including `{}`. Empty
content is invalid, distinct from an optional missing file. Nullable values and
full signed/unsigned 64-bit numeric JSON survive round trips. SafeInteger limits
apply at schema admission in every layer. QML Int64/UInt64 remain decimal strings;
files contain exact JSON numbers. URL strings are decoded by the file adapter
according to the schema, recursively within lists/records/maps, while the raw
tree retains strings and unknown data. Native setters still require actual QUrl.

Parse/IO errors leave the last-good layer intact and emit `sourceError` with
sourceId, file, phase, path and (for parsing) line. Startup failure prevents Ready.
Schema-invalid input follows FieldFallback, RetainLastGood or RejectInput as
declared; RejectInput at initial file load fails initialization. Raw invalid data
is retained separately from admission. Diagnostics contain no input values.

`initializeIfMissing(path, value)` registers conditional user intent. Presence is
raw presence, including false/zero/empty/null/invalid. At publication, the worker
checks the fresh file under the same cooperative lock used by normal writers;
an intervening present value wins over initialization. Its admission result is
not a persistence receipt. Save/flush/close (or autoSave) settles the condition,
publishes the authoritative projection and reports coverage. If all conditions
were already present, the bytes are untouched and save returns nothing_to_save.
Ordinary explicit equal assignments remain writes; reset removes raw overrides.

## Publication and concurrent processes

All parsing, hashing, path-intent comparison, locking, staging, IO and readback
run on the store's worker. Saves capture the input prefix, never the facade.
For each dirty file the worker resolves the final target and obtains an exclusive
flock on `<resolved-target>.lock`
(2-second bounded acquisition), freshly reads it, compares each intended path
against its accepted baseline, rebases disjoint foreign edits and preserves
unknown raw data. Atomic records/lists and namespace resets conflict as whole
paths; foreign changes to namespace structure cannot be overwritten as disjoint
leaf edits. Overlap reports failed/path-conflict and retains the input and failed
attempt. Later saves do not silently retry failed work or recapture it.

Publication uses a unique 0600 staging file beside the resolved target, a complete-write
loop, fsync, fresh identity check, atomic rename, parent-directory fsync and
exact byte readback. Existing permission mode is preserved. New files stay 0600.
The confirmed revision records the resolved target path, device/inode/size,
nanosecond mtime/ctime and SHA256. Configured paths may contain symlinked files
and parent directories, including relative links and absent final targets. The
backend resolves the complete chain (bounded at 40 links), and staging, locking,
rename and readback all use the same pinned resolved target. Publication leaves
the configured symlink intact. A retarget during the transaction fails preflight,
or reports uncertain if discovered after publication. Two source descriptors
aliasing the same resolved target are rejected, including a later alias collision.
Hard-linked regular files may be read, but publication fails with
`hard-linked-target`: replacing them would silently break the link.
Paths are app-owned configuration locations, not a sandbox against hostile
directory mutation. Cooperating writers must retain the sidecar lock file;
never unlink it as cleanup.

The lock/revision protocol coordinates participating writers, including separate
processes. Nonparticipating editors can still race the last check and rename:
this is not a universal filesystem CAS, all-file transaction, adversarial
filesystem boundary or universal power-loss guarantee. Detected preflight races
fail known; a failure after rename or mismatching readback fails uncertain.
Partial shard success remains failed/partial with per-file outcomes. Confirmed
covered revisions never include failed publication as success. Filesystem/mount
semantics determine the durability of fsync; network filesystems are not covered
by the Linux local-filesystem verification lane.

## Watches and automatic persistence

| # | Mode | Behavior |
|---|---|---|
| 1 | Off | No startup read or automatic reload; explicit reload and write preflight still read. |
| 2 | StartupSnapshot (default) | Read once, then only explicitly requested reload. |
| 3 | Live | Watch files and parent directories (nearest existing ancestor for absent directories); handle editor replace, deletion and recreation. |

Watch hints coalesce on the worker. Watch coverage includes the resolved file and
its parent plus the parents of symlinks in the resolution chain. Externally
replacing a file or directory symlink causes re-resolution and re-arms coverage
for the new target; edits to that target reach consumers. Only changed resolved
target identities are read;
accepted reads and exact own publication/readback establish watch authority.
Failed reads/readbacks never become accepted echo-suppression identities. A hint
whose full stat identity equals the confirmed own readback does no content read,
parse, admission or facade publication. The cached revision includes the already
confirmed hash; changed metadata requires fresh content/hash verification, with
no time-window suppression. Same bytes at a new inode are re-observed. These are
normal local-filesystem identity checks, not protection against privileged
metadata forgery. Only changed values/provenance generate GUI deltas. Reloads
overlay outstanding intent without replacing its original conflict baseline.

`reload(callback)` explicitly reads the named file sources and publishes one
coherent admitted generation. Results use operation reload and status reloaded
or failed; they are not save receipts. Save statuses remain exactly saved,
nothing_to_save and failed. QML failure results include file/phase/path/line.

autoSave defaults true. A worker timer restarts after persistent writes pause
(default 300ms), then asks the facade to register an ordered normal save request
at its reported prefix. Every autosave emits completed and autoSaved with the
normal save result. Read-your-writes and notifications are immediate and never
debounced. Runtime commands never schedule persistence. Explicit save/flush/close
cancel an obsolete pending timer. autoSave false means only explicit commands.
Read-only stores have no dirty persistent routes and never write.

## Consumer destruction and process shutdown

Accepted persistence belongs to the worker, independently of the store QObject.
A process-wide shutdown registry retains one lifetime owner per live worker;
it contains no shared configuration state and never relies on a store QPointer
to decide settlement. Namespace/facade caches and callbacks remain consumer-owned.

Destroying a store drops its callbacks, seals its ingress and hands off its
already-queued input prefix. With autoSave enabled, or an outstanding explicit
save/flush/close, the detached worker stays alive until that work settles. It
joins an existing close, or appends a close/flush fence covering the reported
prefix; no facade scan or recapture occurs. Destruction is nonblocking and cannot
cancel an accepted publication. With autoSave false and no outstanding persistence
request, destruction does not turn manually unsaved edits into a save.

One registry hooks both QCoreApplication::aboutToQuit and qAddPostRoutine. It
requests settlement of every live worker, even after its consumer was deleted
before quit or during the active event drain. The post routine also covers
returning from main without an exec()/quit event. The application's event
dispatcher is still available at this cleanup point on the supported Qt target.
The registry processes events, allowing heartbeats and consumer destruction to
continue, while publication/IO/locking stay on each worker.

All workers and both cleanup hooks share **one five-second shutdown budget per
QCoreApplication lifetime**, measured from the first drain. The exit hook never
adds another five-second wait after a quit timeout. Unsettled workers are reported
once each to stderr as `configqml: shutdown-timeout: store=... prefix=...`.
This path retains worker ownership rather than cancelling IO, does not claim saved,
and cannot make work survive termination of the process itself. Failed detached
publication is likewise reported to stderr with store, prefix, error code and
phase; a living consumer still receives its ordinary correlated failed receipt.

Ordinary close remains asynchronous. Its original ordered completion follows
publication and actual worker-thread join, including thread-local destructors.
Zero-wait join polling is independent of the store's lifetime; already-Closed
workers need no duplicate save. Use close when a caller needs a receipt before
continuing. Destroyed consumers' callbacks are cancelled, never executed on behalf
of the detached worker. Worker-safe native predicates/hooks must own their captures
for the full operation lifetime, not borrow a consumer which may be destroyed.

## Verification

Build/tests execute only in agora VM 806 dev-niri-configqml, workspace
`/home/tester/Projects/cqm3`, through `/home/user/.local/bin/agora-niri ssh 806`.
Set TMPDIR to that workspace's tmp and LANG=C.UTF-8. Full CTest includes native
restart/raw-preservation, real independent writer processes, faulty publication,
watch/delete/recreate/retarget, conditional presence, 29-shard echo/fan-out and
GUI-event timing coverage. Process tests retain each child's status, exit code,
stdout, stderr, conflict receipt and final bytes; real exit probes cover Ready,
Closing, Closed, manual-close, failed publication and destruction while Closing,
deletion inside the active quit drain, pending-autosave deletion before quit,
returning from main without quit, multiple stores, and the shared timeout report.
The separate QtQml-only consumer imports the plugin cold,
observes bindings, persists exact uint64/lists, restarts and observes external
edits and autoSave. Run Debug with QT_FATAL_WARNINGS=1 and the ASan/UBSan lane.
