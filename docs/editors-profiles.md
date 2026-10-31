# Editors, scopes and portable profiles

These APIs share the existing ordered `ConfigStore` worker, schema, routes and
publication protocol. They do not create another layer/merge owner or synchronize
ordinary detached QML copies. Preferences and state belong in separately declared
stores when their lifetimes/policies differ; application/domain effects remain
outside configqml.

## Detached drafts

```qml
const editor = store.draft([["bar"], ["appearance", "theme"]])
editor.values.bar.position = "top"
// Bind a preview to editor.values, while the application still reads store.values.
editor.apply(function(result) {
    // saved / nothing_to_save / failed, with normal per-file receipts
})
// Or editor.discard(), which refreshes the base and explicitly drops these edits.
```

`draft()` without paths selects all writable declared leaves; a supplied path may
select one leaf or a namespace. Invalid selections fail visibly. Create a draft
at Ready after selected local writes settle; otherwise creation reports
`unsettled-draft-base`. A draft owns its detached baseline and edits. Getters on
its selected values immediately read edits, including property-attached list
mutations. Unselected leaves are undefined/nonwritable. Explicit equal assignments
are dirty intent. A native draft exposes `value`, `setValue`, `presentation`,
`dirty`, `conflicted`, `busy`, `apply` and `discard`.

Apply queues **one grouped input** and combines its admission and persistence
into **one result**. The object thread passes already-owned checked values, not a
second full-tree capture. The worker checks the selected per-path content revisions
and ingress generations, stages all edits in a candidate layer owner, and accepts
all or none. It freshly reads file sources before checking draft fences; a foreign
edit does not need a prior watch/reload to be detected. Unrelated raw paths in the
same shard do not invalidate a draft. Raw changes beneath an atomic setting,
masked changes, invalid raw changes and equal-valued local intent do invalidate
that path's base. No stale publication may overwrite newer ordinary local input.

The accepted store generation is installed coherently before completion. This
does not promise an all-file filesystem transaction: partial publication remains
failed/partial with truthful route outcomes. Preview/dirty edits survive admission,
revision, IO and partial failures. `conflicted` is sticky until discard or a
successful apply; there is no silent overwrite/automatic retry. Discard explicitly
rebases to current store values. Writes, another apply, and discard are refused
while apply is busy. The apply callback observes the settled draft state; ordinary
store completed signals precede callbacks as before. Deleting an editor cancels
its callback delivery, never its already accepted publication.

An admission-rejected attempt settles only its own covered ingress generations;
it does not change the draft baseline or drop edits. Correcting the edit can then
apply again without discard. Foreign accepted intent (even equal values), changed
content and newer local ingress still conflict. An accepted group with a failed
IO/partial receipt does not get this rejection settlement or erase failure history.

Worker-accepted per-path **intent revisions** are distinct from object-thread
ingress generations. Every accepted writer (direct commands, draft groups and
worker-resolved profile imports, including equal/no-op intent) advances the same
authoritative fence. A profile import never invents an object-thread generation;
delayed import results therefore cannot overwrite newer local setters. Disjoint
paths do not conflict merely because a different path received intent. Native
`info().intentRevision` and QML `info(path).intentRevision` expose the worker
counter (a decimal string in QML), separately from latestInput/workerCovered,
pathGeneration and contentRevision.

Draft preview starts from effective values, even under a runtime/environment mask.
The editor may then show a different pending value. Apply still writes the user
layer; the ordinary masked getter remains effective, and provenance names its mask.
Successful apply refreshes the preview from the settled effective store. Apply's
normal save covers the already-reported store prefix, including earlier accepted
user writes. Autosave never sees un-applied draft edits.

## Scoped handles

```qml
const plugin = store.scope(["plugins", pluginId])
plugin.values.enabled = true
plugin.save(function(result) { /* normal shared-owner receipt */ })
plugin.detach()
```

Paths on explicit handle commands are relative; namespace getters expose that
scope's subtree. Opaque IDs are complete segments, including dots/slashes/Unicode;
they are never split or treated as filenames. The host declares schema namespaces
and nonoverlapping file routes before store construction. Use `encodeScopeKey`
when constructing storage components. There is no guessed route or new worker.
Nested read-only handles cannot upgrade their parent's permission. Detach/close
of a handle cancels only its delivery and leaves the shared store, sibling handles
and already accepted work alive. A detached QML handle reports Closed and refuses
writes. These conveniences are not an in-process security sandbox.

Native `store.scope(prefix)` returns a `ConfigStoreHandle` with relative value,
info, listModel, setUser, save, draft, scope and detach. Native draft paths remain
absolute store paths, including when created by a scoped handle. Handle QObject
parentage determines lifetime, independently of shared persistence ownership.

## Named raw profiles

```qml
store.exportProfile("Laptop", profileFile, [["bar"], ["appearance"]], callback)
store.importProfile(profileFile, [], callback)
```

Native equivalents take `QList<ConfigPath>` and context/callback. All file work,
selection, raw projection, validation and grouped import are worker-owned. Export
uses the JSON backend's shared descriptor-relative atomic publisher with 0600
permissions, reports `exported`
or `failed`, and refuses a store source as its destination. Import uses the normal
saved/nothing_to_save/failed receipt, including partial multi-file outcomes. Paths
must be absolute and clean.

The storage owner resolves the complete export destination with the same bounded
symlink resolver used for configured sources, including dangling targets and
symlinked parent directories. It excludes every configured source's resolved
target, then opens the resolved parent with `O_DIRECTORY | O_NOFOLLOW` and takes
the same bounded cooperative `<target>.lock` as configured-file publication.
The shared publisher creates a unique private staging file with
`openat(O_CREAT | O_EXCL | O_NOFOLLOW, 0600)`, completely writes and fsyncs it,
checks resolution/source exclusion and the descriptor-relative final identity,
then uses `renameat` on that same directory descriptor and fsyncs the directory.
Profiles, configured-file replacement/deletion and no-replace migration backups
share this publisher; profiles never fall back to pathname-based QSaveFile writes.
Hard-linked destinations are refused rather than broken by replacement. Retain
the cooperative sidecar lock file; it must not be unlinked as cleanup.

Once the parent is open, a later parent-component swap cannot redirect staging
or publication: the export either fails validation or lands in the already-opened
directory, even if that directory has been renamed. This does **not** implement
component-by-component traversal anchored at a trusted root. Components above the
opened parent can change during resolution/open; `O_NOFOLLOW` protects only the
final parent component. Source exclusion and identity rechecks do not make those
earlier components race-free. This is the same residual model as configured-file
publication, not a sandbox against arbitrary hostile directory mutation.

Import accepts regular files only (symlinks to regular files are allowed), refusing
FIFOs, devices and directories without waiting for input. The opened file identity
is checked too. `ConfigStoreOptions::profileMaxBytes` / QML `profileMaxBytes` is a
construction-time positive byte limit, default **16 MiB** (16 × 1024 × 1024), including
whitespace. Import checks the opened size and bounds every read, including files
that grow after opening. Excess reports `profile-too-large`; non-regular input
reports `profile-file-type`; a nonpositive limit reports `profile-size-limit`.
Refusal does not change settings or block subsequent worker commands.

The version-1 JSON envelope contains `format: 1`, a nonempty `name` and `entries`.
Each entry contains a segment-array `path`, boolean `present`, and raw `value`
when present. Only **declared raw user overrides** on selected writable routes are
exported: never flattened defaults, environment or runtime values. An absent
selected override is recorded explicitly and resets that path on import. Paths
outside the imported selection are untouched. Import is one coherent grouped
admission/publication, with no implicit cross-store coordinator.

Export is a whitelist. Unknown keys, including unknown fields inside atomic
records, never leave the store in a profile. `sensitive`, `forbidden`,
`machineLocal`, `SecretRef`, and inherited group exclusions are omitted. If an
atomic record/list/map contains any excluded schema descendant, that whole
setting is excluded rather than fabricating a partial record. Nullable null stays
null. Maps retain opaque keys; declared record fields inside maps/lists are
whitelisted recursively. Invalid/duplicate/unsafe profile entries and unknown
atomic record members are refused at import before changing any setting.

Every whitelisted export value must pass its descriptor's shape, type and
constraints, including scalar elements nested in lists/maps/records. Invalid raw
overrides fail the entire export with `profile-admission` before publication;
they are never coerced to empty containers, silently reset or emitted unchecked.
An existing profile and the original raw sources remain unchanged on refusal.

Profile files are portable preference snapshots, not full-store backups. Raw
unknown/invalid data remains preserved in the original source files by the normal
backend. Applications migrating legacy mixed preference/credential files must
deliberately classify and move credentials with their own provider.
