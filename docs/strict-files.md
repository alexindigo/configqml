# Strict source conflicts, evidence and empty shards

The three Strict settings are independent: `unknownData: Strict` closes source
data, `conflictPolicy: StrictSourcePreimage` fences whole-file foreign changes,
and a schema node's `admissionPolicy: Strict` rejects invalid layer admission.
None implies either of the others. Unknown-data removals retain role
`unknown-data` evidence before publication; see [json-files.md](json-files.md).

`ConfigJsonFile::conflictPolicy` / `JsonFileSource.conflictPolicy` defaults to
`PathIntent`: the existing cooperative writer protocol merges disjoint foreign
path edits. Select `StrictSourcePreimage` when a save must refuse *any* foreign
content change to the original source, including unknown keys and formatting,
or changes to configured file/parent links or directory identity. Declaration
metadata compiles once. A final file's inode/timestamps alone do not conflict:
exact original bytes and presence under the lock resolve frozen ordinary work
and retained-group retries. Semantically equal JSON with different bytes still
conflicts. Evidence keeps both observations and their distinct revisions.

Strict stores require an absolute clean `evidenceDirectory`. Pending intent and
retained groups capture original presence, exact bytes and the resolved revision.
Foreign reloads can update accepted disk presentation but cannot rebase that
preimage. The JSON worker compares it again under the resolved target's sidecar
lock. Only correlated own confirmed publications advance causal descendants.
Strict conflict is `source-preimage-conflict`; the original intent remains unsaved.
This is cooperative locking and identity checking, not universal filesystem CAS.

Within one save, sibling strict routes may share newly created parent directories.
The backend records only its own successful directory creations, with exact
device/inode identities and open descriptors retained until that save settles.
Those witnesses can extend an earlier missing-parent chain for another route in
the same save, including retained-group routes. A directory created by another
writer, an existing directory, a replacement inode or a configured symlink change
does not qualify. The allowance ends with the save; it is not a lifetime exemption
for other captures or later requests. Both strict publication policies use this
same parent-identity owner; each file still locks, publishes and reads back on its
own route, with truthful partial failure if a foreign parent change is detected.

## Pending rebase (0.5.0)

`ConfigConflictPolicy::StrictPendingRebase` / `JsonFileSource.StrictPendingRebase`
is an additive, opt-in policy requiring `evidenceDirectory`. Ordinary pending
intents inside a save's input prefix may rebase while their source is unfrozen,
as defined below. Reserved work and retained captures never rebase. Watch
and explicit reload observations update presentation, not the pending preimage.

Rebase occurs under the resolved target's sidecar lock against the fresh file.
Its link and parent chain must still match the capture; a final inode/mtime
change is allowed, including exact-byte ABA. Changed bytes must be decoded,
retained as `foreign`, admitted to the disk layer and published as an `external`
source generation with pending intent overlaid. Locked preflight uses the same
acceptance owner as reload; it posts that generation before `own_save`. Evidence
or admission failure prevents publication. A changed file discovered only at
preflight receives the same treatment as an earlier accepted watch/reload.

The candidate starts from that fresh raw file. Each ordinary edit is compared
at its fence against the original dirty baseline. Disjoint edits preserve all
foreign siblings. At the same leaf, local intent wins over a foreign addition,
change or deletion, including atomic lists, records and maps with opaque/open
members. Missing object ancestors are recreated only along the local path;
foreign sibling deletions stay deleted. A changed non-object ancestor refuses
with `path-conflict`. Namespace reset fragments still fence the entire subtree:
any foreign change inside it refuses rather than partially resetting it.

Conditional initialization uses fresh presence, and cleanup removes only empty
namespace scaffolding. A foreign deletion is honored: a write recreates only
its applied leaves, while a reset against an absent file is `nothing_to_save`.
After a real rebase, an exact candidate already on disk is covered without
writing. Unknown-data closure and `deleteWhenEmpty` run after applying the
eligible edits as usual.
These rebase-only no-write shortcuts require F to differ from P. When F equals P,
publication follows StrictSourcePreimage byte for byte, including explicit
equal-value rewrites and resets against an unchanged absent file when
`deleteWhenEmpty` is false. A required (`optional: false`) file
still enforces startup/reload presence, but its foreign deletion at pending-rebase
preflight is honored: a local write recreates only its leaves, and reset can leave
it absent. Required presence is not a publication deletion veto.

A failed attempt before a successful locked fresh read (for example lock failure,
target validation or failure to retain original evidence) does not freeze ordinary
work. Once a fresh file has been read, a refusal with no publication effect keeps
the original capture P and baseline B, including parse/admission and structural
conflicts. Restoring P resolves that work through the strict content path. Only
an actual or uncertain publication effect freezes against its computed preflight
base F and retains that attempt's overlap classification. This no-effect pin rule
refines the original pending-rebase proposal so refused work can be resolved.

While a source has frozen ordinary work, all eligible ordinary edits to that same
file, including newer never-attempted edits, use that frozen preimage strictly.
They cannot publish around it by rebasing; other sources remain independent.
Later saves, including autosave, may retry the strict capture. Exact-byte
restoration resolves it despite a changed final inode/mtime. After an uncertain
effect, ordinary work has no implicit own-output recovery: the computed preflight
base must be restored before retry. Retained groups keep their explicit recovery
protocol. Nothing is recaptured from the facade or discarded.

Every attempt retains `original`, fresh `foreign`, `published` and `readback`
observations where available. Evidence metadata keys are unchanged. A route fact
adds optional native `rebasedOnto` (QML revision string, empty when absent) and
`overriddenPaths` (absolute schema paths, sanitized like unknown-data paths).
They describe an ordinary rebase or its frozen retry. Covered no-write rebases
can carry a route fact while the aggregate remains `nothing_to_save`.
After an actual publication effect, each overridden leaf emits `sourceWarning`
with phase `rebase`, code `foreign-value-overridden` and foreign evidence.
Delivery is capped at 64 paths plus `foreign-value-overridden-truncated`.
An overlap is reported only when the final candidate actually differs from F
at that leaf; a matching foreign value or matching absence is not overridden.
Known failures before publication emit no override warning; uncertain effects
still report their actual publication facts. An exact-candidate no-op has no
overridden paths or override warning.

Retained/stopped groups, frozen ordinary attempts, changed non-object ancestors,
namespace overlaps, retargets, parent replacement, hard links, lock failures,
parse errors and evidence failures still refuse. Writes after preflight are
checked by publication identity validation and readback, with known or uncertain
failure as appropriate. The protocol remains cooperative, not filesystem CAS.
In a multi-source save, confirmed earlier own routes publish their own source
generation before a later source's foreign preflight observation is admitted.
Unchanged already-accepted preflights do not create an extra generation or a
no-op migration receipt. Identical foreign observations within one attempt share
one retained reference; different attempts and roles keep independent evidence.

## Retained observations

Each observation gets a unique 0700 directory in `evidenceDirectory`, with exact
0600 `bytes` when available and value-free 0600 `metadata.json`. An absence has
no byte artifact. Unavailable bytes and unknown presence are explicit. Artifacts
and directory entries are fsynced before issuing an evidence reference; existing
artifacts are never overwritten. Source states and results carry references.

Startup, original preimages, fresh preflight, published output, readback and
late/unfamiliar observations are retained independently of GUI cache eligibility.
Metadata correlates the store, source, request, group and attempt when applicable;
it includes phase, observation order, presence, bytes state, revision and role.
An own publication witness is separate from confirmed durability. A failed
readback can therefore retain both the actual own output and a foreign replacement.
Evidence failure reports `evidence-retention`, including after known publication;
successful route facts and covered revisions are still reported truthfully.
This also applies to versioned migrations: an underlying publication or conflict
error remains in its route entry while the overall error is `evidence-retention`.
Only confirmed readback contributes covered revisions; uncertain routes stay
uncertain even when earlier shards succeeded and the aggregate result is partial.

## Opt-in deletion

`ConfigJsonFile::deleteWhenEmpty` / `JsonFileSource.deleteWhenEmpty` defaults false
and requires a writable source. After reset, an opted-in file with no raw choices
is deleted instead of encoding `{}`. Library version metadata alone is not a
choice. Cleanup removes only declared empty namespace scaffolding reached by
reset: atomic empty records/lists, false, zero, null and unknown keys remain raw
choices. Resetting an individual leaf cannot delete a nonobject ancestor;
explicit namespace reset can remove that whole raw subtree.

Deletion uses the same preimage, lock, hardlink refusal and target validation as
publication. Only the resolved regular target is unlinked; configured file/parent
symlinks and the sidecar remain. `saved` follows directory fsync and confirmed
absence, with an absent revision on the normal source route. Already absent with
no write is `nothing_to_save`. Retargets fail `empty-delete-target`; after-effect
sync/readback faults fail with truthful uncertainty and retained evidence.

Live watches re-arm against the nearest existing ancestor and link chain, so a
later ordinary write can recreate the same resolved target and external recreation
updates consumers. Reset reveals the applicable lower layer immediately after
worker admission and after restart; conditional `fallbackFor` selection remains
the existing whole-raw-object rule.
