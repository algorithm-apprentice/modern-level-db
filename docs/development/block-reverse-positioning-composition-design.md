# Block Reverse Positioning Composition

## Status and scope

Proposed recursive follow-up to the
[production composition plan](production-function-composition-plan.md).
Source-read baseline: `dde6969`. Recovered-counter implementation #238 is
pending on different source/test/design paths; this design has no dependency on
its counter changes.

Scope is Block::Iterator Prev/SeekToLast and three private children in
`src/table/block.cc`, directly dependent private declarations in block.h,
existing block characterization tests and this document.
No entry/varint decoder, layout creation, checkpoint/lower-bound helper,
comparator policy, table/engine iterator, allocation, coverage or benchmark
change. These unchanged children remain separate recursive frontiers.

## Predecessor positioning

Prev first captures original current_ as before. A new
FindPreviousRestart(original, found) borrows an explicitly false-initialized
boolean outcome and performs the original restart loop in order:
decode/bounds-check restart_index_; if offset < original, set found true and
return success; if restart_index_ is zero, Invalidate and return success;
otherwise decrement and continue. Restart corruption still calls existing
Corruption at the original logical site.

The outcome names "preceding restart found" versus successful beginning.
Do **not** infer it from valid() or add a new valid-position guard. That would
change previously unguarded positioning behavior beyond this extraction.
No unsupported-caller contract is added or certified. The child changes only
the original fields and has no owner, callback or replacement offset container.
The parent propagates a mutable failed search Status by move; successful
not-found returns success before seeking. New Status/bool boundaries are not
claims of identical physical move/elision counts.

For found, Prev retains the original const sought Status from
SeekToRestartPoint(restart_index_) and its existing bounded-seek exclusion.
Only successful seek calls ScanToPreviousEntry(original). This child preserves
the original do/parse/failed-copy/next_ < original loop and stops on the entry
whose next offset reaches original. Do not change to key comparison, an
unconditional extra parse or eager validation of unvisited bytes.

## Last-entry positioning

SeekToLast retains BeginPositioning, zero-restart invalidation/success, and
SeekToRestartPoint(restart_count - 1) in order. The const sought Status stays in
its parent scope across the borrowed scan call.
ScanToLastEntry preserves the original do/ParseNextEntry/const failure copy/
valid() && next_ < entries_end loop. Empty data with an allowed restart still
invalidates successfully through the original parse; reached malformed entries
still clear state through Corruption. A positioning call resets prior error as
before; Prev itself does not gain BeginPositioning.

These two scans have different stop predicates and jobs. Do not introduce a
generic scan callback, alter parser/state-commit ownership or force them into a
helper that changes read/evaluation/error lifetimes. Iterator owns its key,
value view, layout and error throughout; block/comparator owners and all
original const parse failure-copy sites remain unchanged.

## Five-definition recursive ledger

| Definition | Responsibility and children/stopping reason |
|---|---|
| `Prev` | Capture original position, find a preceding checkpoint, seek and scan to the immediate predecessor. |
| `FindPreviousRestart` | Walk backward over restart offsets to a strict predecessor or beginning while retaining corruption/index mutation order. |
| `ScanToPreviousEntry` | Reconstruct forward through existing ParseNextEntry until the next entry reaches the original offset. |
| `SeekToLast` | Reset positioning, handle absent restarts and seek the final checkpoint before scanning. |
| `ScanToLastEntry` | Reconstruct the final restart suffix through existing ParseNextEntry until physical entries end. |

Existing RestartPoint, Invalidate, Corruption, SeekToRestartPoint and
ParseNextEntry are named children; do not add aliases around their single calls.
The scans name distinct domain targets and preserve their stateful predicates,
not a blanket exemption for reverse algorithms.

## Characterization before extraction

Retain direction switching, moved backing owners, extended/overflow varint
compatibility, growing/shrinking/internal keys, empty/zero restart blocks,
out-of-range restarts, malformed predecessor scan and positioning recovery.
Add two public cases on unchanged production using existing byte fixtures:

1. Literal empty/binary prefix-compressed keys and values traverse backward
   across restarts with exact copied observations, reach successful beginning,
   reposition to last and change direction. RecordingComparator observes no
   comparisons for last/predecessor operations; do not derive the golden order
   through a producer/sorted model or retain invalidated borrowed key views.
2. Reached malformed predecessor entries, earlier out-of-range restart offsets,
   and a malformed last suffix return exact `"bad entry in block"`, invalid
   position and cleared key/value; supported positioning recovers to the
   fixture's reachable valid prefix/suffix. Keep empty/zero-restart success
   distinct, no unsupported positioning calls or fabricated spans.

Run the anchored coupled native Windows selector:
`^(BlockBuilderTest|BlockTest|BlockIteratorTest|BlockModelTest|TableTest|TableNativeTest|TableBuilderTest|DbIteratorTest|DbIteratorSamplingTest|MergingIteratorTest|LevelIteratorTest|CompactionTest|DatabaseTest)\.`
Build both unit and Windows lock-child targets. Measure original unique
classname/name identities after additions, require additions and no
failure/error/skip; compare final coupled XML exactly.
Between the two source topics run
`^(BlockBuilderTest|BlockTest|BlockIteratorTest|BlockModelTest)\.`
Do not reuse prior block-positioning counts as this new baseline.
Owner/const-Status lifetime and physical allocation/destruction equivalence are
source-inspected where no observer exists, not inferred from output equality.

## Review and delivery

Independent design review and acceptance precede characterization.
Independently review original-source cases before production edits.
Use two small topics: predecessor checkpoint/scan composition, then final-entry
scan composition. One non-draft implementation PR may contain both.
Combined independent review checks the accepted diff, actual parser/state/
restart/caller paths, explicit found outcome rather than a new valid() gate,
const sought/parse lifetimes and copy sites, exact predicates and failure state.
Preserve coverage annotations without new exclusions.
Require all exact reviewed-head checks, actual hosted Windows Debug selected
name parity, normal merge and byte-exact reviewed changed paths in main.
Docs-only acknowledgements are not runtime proof or whole-file certification.

## Independent design review

Bounded read-only review found no significant issues. It traced restart search,
scan predicates, corruption/invalidation and const Status lifetimes/copy sites,
including table/level/merging/database caller paths. The explicit found outcome
does not add a valid() guard or BeginPositioning to Prev. New Status boundaries
are acknowledged rather than claimed to preserve physical move counts.
No edits, builds, tests or further delegation occurred. Private declarations,
implementation equivalence, native identities/counts and hosted parity remain
unverified at this design review.
