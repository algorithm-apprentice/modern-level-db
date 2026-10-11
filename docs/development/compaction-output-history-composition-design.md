# Compaction output transactions and visible-history composition

## Status and scope

Accepted design for C01/C02/C03/C04 from the
[production composition plan](production-function-composition-plan.md).
Design-only #193 passed independent review and all 20 exact-head checks before
merge. The separate implementation record below follows that accepted design.
Scope is `src/engine/compaction.cc` and its direct characterization.
Compaction selection/expansion, iterators, native filesystem policy, MANIFEST
installation and benchmarks remain outside this batch.

Preserve the existing algorithm rather than introducing a compaction framework.
Small output, overlap, base-level and history owners describe independent
monotonic domain state. Borrowed input spans remain paired with their caller's
version/input/table-cache owners.

## Original processing order

RunCompaction initializes grandparent/base-level/output owners, then seeks input.
For each valid entry it calls before_entry, obtains the key, advances
grandparents even for dropped entries, finishes an open output at an overlap
boundary, parses the internal key, updates user history, decides retention,
records this sequence even if dropped, adds only retained entries, then advances
input. Preserve all short circuits and this order.

Input-move failure wins over final output finishing. After successful traversal,
finish any open output, construct CompactionEdit, return it without filesystem
sync if there are no outputs, otherwise sync the directory once and add verified
files to the edit. The caller still installs the edit.

## C04: monotonic overlap and deeper coverage

Extract `GrandparentOverlap::AdvancePast(key)` containing the existing loop and
first-key bookkeeping. ShouldStopBefore composes advancement with the original
strict `overlapped > limit` test and reset. No saturating arithmetic, changed
overflow policy or different key comparison.
The first key's passed grandparents do not count. Later passed bytes count even
when no output is open. Exceeding the limit resets overlap regardless of whether
an output is open. A dropped entry still advances the same state.

Extract `BaseLevel::LevelContains(level, user_key)` containing one level's
original monotonic cursor scan. Return true for the inclusive matching range,
false for a gap/end, with largest comparison before smallest and no extra
comparison. IsBaseLevelForKey composes deeper-level scans and stops on the first
containing file; no eagerly scanned later levels.

## C01/C02: output acquisition, append and completion

Outputs::Open(key) allocates the original hook number, builds its path, opens
the writable file, emplaces TableBuilder and stores smallest key in that order.
No builder is created after open failure. smallest assignment remains after
builder creation, retaining exception cleanup. Outputs::Add composes conditional
Open, Append and existing target-size completion.

Outputs::Append(key,value) stores largest, calls AddTrusted and returns its
Status directly. On Add failure, target-size checking and Finish do not execute.
Size threshold remains inclusive and uses current builder file_size, not final
footer-inclusive physical size.

Outputs::FinishBuilder returns Result<uint64_t>: call builder Finish, obtain
file_size even on failure, reset builder before checking status, retain the
original first failure and return the successful size. Move the original
Status error into this new envelope. Outputs::Finish copies that error into
its returned Status, retaining the original Finish-local copy; its caller
retains the second existing Error copy. Do not return before resetting the builder,
add a Close call or recover a sticky error.

Outputs::MakeMetadata(size) decodes stored smallest then largest with existing
TakeTrusted, and returns named local FileMetadata aggregate with original number,
size and owned bounds. Outputs::Finish composes FinishBuilder, the original
table-cache Find and metadata recording. Keep the successful Find handle in
Finish through decoding and finished_.push_back, including exception unwind.
Do not extract verification into a helper whose returned Status releases that
handle too early. Original Find failure error copy remains; no metadata is
recorded when finishing or verification fails.

## C03: history, entry composition and final edit

Introduce private `VisibleHistory` borrowing the user comparator, holding the
same optional owned user-key bytes and optional preceding sequence.
`StartUserKey` compares current parsed user key with stored bytes; only a new
user key emplaces copied bytes and resets preceding sequence.
`ShouldDrop(entry,snapshot,base_level)` composes StartUserKey with the original
shadowing/deletion predicate, then always records this parsed sequence. Retain
the predicate's exact short circuit: shadowed records do not call deeper-level
coverage; only an unshadowed deletion visible to the oldest snapshot does.
Snapshot equality and the tombstone's own visibility remain inclusive.

`ProcessEntry` composes before_entry, overlap advance/conditional finish, parse,
history/drop and retained append. Input::Next stays visibly in RunCompaction;
ProcessEntry does not advance input or let borrowed key/value escape. Dropped
entries never request input.value(). Status failure copies currently created
in RunCompaction move into ProcessEntry; new parent propagation moves, without
extra Error copies or string reconstruction.

`FinishCompaction` composes conditional final Finish, CompactionEdit construction,
empty-output shortcut, directory barrier and output-file edit assembly. Parent
checks input movement before calling it. Preserve exact original status error
copies inside the helper and move the new Result at return boundaries.
Keep Outputs alive throughout assembly and final returned edit construction.
No generic callback/visitor, new error taxonomy, heap history owner or allocated
transaction framework.

## Recursive implementation ledger

| Definition | Domain responsibility |
|---|---|
| `GrandparentOverlap::AdvancePast` | Consume passed grandparents and establish first-key bookkeeping. |
| `GrandparentOverlap::ShouldStopBefore` | Compose advance with overlap threshold/reset. |
| `BaseLevel::LevelContains` | One deeper level's monotonic range admission. |
| `BaseLevel::IsBaseLevelForKey` | Compose short-circuited deeper-level coverage. |
| `Outputs::Open` | Acquire a numbered output builder and its initial bound. |
| `Outputs::Append` | Update final bound and append the trusted entry. |
| `Outputs::Add` | Compose acquire/append and size-triggered completion. |
| `Outputs::FinishBuilder` | Finish and reset builder, retaining size/error order. |
| `Outputs::MakeMetadata` | Construct owned output bounds/metadata. |
| `Outputs::Finish` | Compose builder completion, pinned verification and recording. |
| `VisibleHistory::StartUserKey` | Transition owned history when user key changes. |
| `VisibleHistory::ShouldDrop` | Compose snapshot retention decision and preceding-sequence update. |
| `ProcessEntry` | Compose one input record's hook/overlap/parse/retention/output work. |
| `FinishCompaction` | Compose final output completion and durable edit assembly. |
| `RunCompaction` | Compose state setup, checked input traversal, entry processing and finalization. |

Field accessors/constructors retain original behavior; every introduced/changed
definition gets a ledger row. NewCompactionIterator, output metadata's existing
decode primitive, table/cache/iterator internals and CompactionEdit remain
subsequent recursive frontiers. A batch is not whole-file certification.

## Characterization before extraction

Existing CompactionTest covers real two-level merges, L0 ordering, lazy input
I/O/cache-fill options, snapshot/tombstone history, empty outputs, exact size and
grandparent splits, dropped-entry overlap, malformed keys, hook stop, input-move
and every-filesystem-operation failures. Retain these and directly coupled
CompactionPickerTest/TableBuilderTest.

Strengthen error tuples to include codes and verify processing does not proceed
to output completion/directory sync after final input-move or hook failure.
Add mixed hook failure/malformed key to prove hook priority.
Add malformed key at a grandparent boundary with an open output, plus an injected
Finish failure there: completion failure must precede parsing the malformed key.
Assert exact boundary operations rather than just generic Corruption.

Use snapshot equality with an earlier visible retained value and later shadowed
deletion to prove no deeper comparator calls for the shadowed branch. Track
only uniquely identifiable later/deeper key bounds, not all comparisons made by
table writing. Include a deletion newer than the oldest snapshot with deeper
files; it must stay without any unnecessary coverage scan.
Existing tests already cover strict overlap equality and passed bytes with no
open output; preserve their exact outputs.

Validate output metadata owns its smallest/largest after input, compaction,
cache and filesystem source release. Existing output tests read values but do
not alone prove metadata ownership.
Do not claim exact native handle destruction or allocation-injection evidence
without an actual oracle; inspect successful verification pin scope in review.

Record exact unique JUnit before/after selected sets, added names present and
no failures/errors/skips. Rebuild actual modern_leveldb_unit_tests for each
slice. Existing formatting/docs/whitespace checks and authoritative exact-head
remote CI/coverage remain unchanged. No new exclusions or threshold reductions.

## Review and delivery

Independent design persona review precedes design-only publication/merge.
Characterization, overlap/coverage, output transaction and history/parent slices
are separate small topic commits in one coherent implementation PR.
Independent combined implementation review follows focused parity and precedes
publication; all applicable exact-head checks precede normal merge.
Broader production recursive completion still requires all remaining frontiers
and the final definition/lambda audit on both native branches.

Independent design review found no significant issue. Its error-propagation
watchpoint is explicit above: FinishBuilder moves into its new envelope while
Finish and its caller each retain the original Error copy. The reviewer
inspected source/tests without executing builds or tests.

## Implementation record

Five characterization cases were added and committed on unchanged production:
tracked deeper-coverage short circuits for shadowed/snapshot-invisible deletions,
metadata bounds after all sources are released, hook-before-invalid-key priority,
overlap-boundary Finish-before-parse priority, and final input-error suppression
of completion. Existing hook/input/filesystem failures now check exact codes
as well as existing messages. A failed boundary Sync still closes via original
Finish, while an unfinished output destroyed after input failure does not call
the fixture's explicit Close; the initial incorrect test expectation was fixed
before the passing original-code run and characterization commit.

Three production topic commits implement monotonic overlap/coverage children,
output transactions, and history/entry/final edit composition.
All fifteen definitions in the recursive ledger above are implemented.
Outputs::Finish retains its cache handle through owned metadata decode and
finished_.push_back. FinishBuilder moves its error, Finish copies it once, and
the existing caller copy now resides in ProcessEntry/FinishCompaction.
New ProcessEntry-to-parent propagation moves. Output-open failure passes through
the nonconst local Status by implicit move; append keeps its existing local
copy, and trusted append is directly returned by the new child.

The named VisibleHistory owns the original key/sequence optionals and is
destroyed before Outputs, preserving owned byte-key cleanup order relative
to output owners. Input Next remains in RunCompaction; finalization runs only
after successful movement. NewCompactionIterator and deeper table/cache,
metadata/CompactionEdit and iterator primitives remain recursive frontiers.

All 50 selected compaction/picker/reverse-picker/table-builder cases pass
before/after with exact unique JUnit case-set equality, all five added names
present, and no failures/errors/skips. The cursor slice passes 18 compaction
cases; output transaction slice passes 37 compaction/table-builder cases.
Each source slice rebuilt actual modern_leveldb_unit_tests in Windows Debug.
This is not allocation injection or direct native-handle destruction evidence.
Independent combined implementation review found no significant correctness,
design or test gap across the four scoped files and independently inspected
the 50-case and intermediate 18/37-case XML sets without executing tests.
Exact-head remote CI remains the delivery gate.
