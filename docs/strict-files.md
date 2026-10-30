# Strict source conflicts, evidence and empty shards

`ConfigJsonFile::conflictPolicy` / `JsonFileSource.conflictPolicy` defaults to
`PathIntent`: the existing cooperative writer protocol merges disjoint foreign
path edits. Select `StrictSourcePreimage` when a save must refuse *any* foreign
change to the original source, including unknown keys, formatting, inode, configured
file/parent links or directory identity. Declaration metadata compiles once.

Strict stores require an absolute clean `evidenceDirectory`. Pending intent and
retained groups capture original presence, exact bytes and the resolved revision.
Foreign reloads can update accepted disk presentation but cannot rebase that
preimage. The JSON worker compares it again under the resolved target's sidecar
lock. Only correlated own confirmed publications advance causal descendants.
Strict conflict is `source-preimage-conflict`; the original intent remains unsaved.
This is cooperative locking and identity checking, not universal filesystem CAS.

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
