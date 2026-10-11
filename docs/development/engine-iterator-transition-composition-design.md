# Engine iterator direction and visibility composition

## Scope

Design accepted in #205 for I01/I02 in the
[production composition plan](production-function-composition-plan.md).
Scope is MergingIterator direction transitions in iterators.cc, DbIterator
positioning/visibility in db_iterator.cc/h and directly related tests.
Level/memtable sources, block iterators, sampling policy and public APIs are
unchanged. No benchmarks, generic callback framework or coverage changes.
Do not edit the shared ledger while pending MANIFEST work owns that path.

## Merge direction invariants

`TurnForward` visits noncurrent children in original vector order, positioning
each strictly after the current internal key, then sets forward_ true.
`PositionAfterCurrent(child)` seeks using the still-current key, advances once
only for a valid equal key, and propagates original error/invalidation policy.
The current child is not repositioned. Only after all other children succeed
does Next advance current and call existing FindSmallest.

`TurnBackward` similarly positions each noncurrent child before the current key
and sets forward_ false only after success.
`PositionBeforeCurrent(child)` seeks, calls Prev for a valid child or SeekToLast
for an invalid child, and retains each original failure priority.
Prev advances current only after this transition succeeds, then FindLargest.
No current-key copy, child reordering or speculative later-child positioning.
The current child owner and its key stay valid through all other child moves.
Existing forward first-child/reverse last-child tie selection is untouched.

Keep Fail at the child failure boundary, invalidating current_ once. New
parent/transition Status envelopes move; do not call Fail again on their error
and add an Error copy. A failed transition does not publish a direction change.
Subsequent explicit seek restores positioning through existing methods.
Bulk Seek/SeekToFirst/SeekToLast and level-source internals remain recursive
frontiers rather than being silently certified.

## User iterator positioning

`AdvanceForNext` composes the original forward skip-key snapshot and internal
Next, or reverse-to-forward flag transition and Next/SeekToFirst selection.
Keep setting forward_ before movement in the reverse branch as originally.
Next then invokes FindNextUserEntry(true); do not clear saved skip key earlier.
The saved key owns the source bytes before any internal movement.
This child copies the original move error into its Status without calling Fail;
Next moves that error into Fail exactly once.

`PositionBeforeCurrentUser` snapshots the current user key and repeatedly calls
Prev, checks internal validity, parses the reached key, then compares until
strictly before that user key. This walk deliberately does not CountRead.
Return Result<bool>: true means positioning succeeded and forward_ became
false; false means beginning reached, valid_/saved buffers were cleared but
the original forward flag remains untouched. Prev returns immediately on false,
instead of calling reverse scanning and changing forward_ again.

For errors, retain the original Error copy inside the child, then move its
private result error into parent Fail; this preserves one invalidation/copy
without a second error translation or newly invented status.
Keep key parsing after the validity check and comparator after successful parse.
Do not replace the walk by Seek: comparator invocation and read-sampling
behavior would differ.

## Entry visibility transitions

`ConsiderNextEntry(parsed, bool& skipping)` implements snapshot visibility, deletion
skip-key update and strictly-later user-key acceptance. Its true return means
the original valid_=true and saved_key_.clear have completed.
FindNextUserEntry keeps parse, CountRead, consideration, internal Next and
failure handling in that order. Tombstones, snapshot-invisible entries and
skipped versions are still counted. Exhaustion clears only the original
valid_/saved-key fields, not extra state.

`ConsiderPreviousEntry(parsed, ValueKind& kind)` first ignores snapshot-invisible entries.
For a visible entry, it returns stop before consuming an earlier user key if a
value has already been found. Otherwise it records kind, clears saved key/value
for deletion, or copies key then value in original order for a value.
FindPrevUserEntry keeps parse and CountRead before this decision, including the
earlier-user stopping record, and advances Prev only if not stopping.
`FinishReverseScan(kind)` publishes the original deletion/no-value exhaustion
state (invalid, forward true, cleared buffers) or valid saved reverse position.

Parsed keys and values continue borrowing their original owners through their
consideration; reverse saved outputs remain owned across internal positioning.
Do not unify different clearing policies into a broad Reset helper.
CountRead, period generation, key/value access and existing Fail remain
unchanged operations or later recursive frontiers.

## Definition responsibility ledger

| Definition | Job and stopping boundary |
|---|---|
| `TurnForward` / `TurnBackward` | Compose ordered noncurrent repositioning and publish merge direction after success. |
| `PositionAfterCurrent` / `PositionBeforeCurrent` | One child positioning transition relative to a pinned current key. |
| Merging `Next` / `Prev` | Compose direction restoration, current move and existing winner selection. |
| `AdvanceForNext` | One user iterator forward-advance/skip-key transition. |
| `PositionBeforeCurrentUser` | Coupled backward positioning across one user history with explicit begin outcome. |
| Db `Next` / `Prev` | Compose positioning and the appropriate visible-user scan. |
| `ConsiderNextEntry` | One parsed entry's forward visibility/skip-state transition. |
| `ConsiderPreviousEntry` | One parsed entry's reverse stop/history state transition. |
| `FinishReverseScan` | Publish reverse scan's selected-value or empty result state. |
| `FindNextUserEntry` / `FindPrevUserEntry` | Compose checked per-entry parsing, sampling, visibility and movement. |

Ordinary assignments, vector copies, comparator and existing iterator calls are
representation primitives, not new single-statement wrappers. Named children
preserve coupled direction/history invariants rather than introducing generic
state-machine/configuration objects.

## Characterization and delivery

Existing tests cover duplicate-child ordering, random direction models, empty
sources, equal-key pass failure, every move failure/reseek, lazy table failures,
snapshot/deletion history, both comparators, malformed keys and exact sampling.
Add test-local move recording around the existing scripted child to assert exact
child/method order for both transitions, equal-key advancement, invalid-child
SeekToLast fallback and immediate stop at a selected failure. Do not modify the
shared scripted fixture or production iterator interfaces merely for logging.

Add explicit first-user reverse-to-forward and beginning-exhaustion behavior,
binary/empty user keys with owned reverse output, and invisible/deleted history
across repeated turns. Strengthen failure code/message and exact sampling
oracles, especially no positioning-walk samples and counting the reverse
boundary entry before stopping. Keep views inside their documented lifetime.
For the reverse-output ownership oracle, use a test-local backing buffer that
is overwritten on child movement: the shared scripted fixture retains entry
storage and alone cannot detect an accidentally borrowed saved value.

Run added characterizations on original production before extraction.
Rebuild the actual unit target; select MergingIteratorTest, DbIteratorTest,
DbIteratorSamplingTest and directly coupled LevelIteratorTest,
InternalIteratorTest, MemTableIteratorTest, CompactionTest and DatabaseTest.
Verify exact unique before/after JUnit sets and added names without failures,
errors or skips. Use separate merge-direction and user-history topic commits.
Independent design review precedes accepted design merge; independent combined
review and exact-head successful CI precede normal implementation merge.
Formatting/docs/whitespace gates and unchanged coverage thresholds apply.

Independent source-grounded design review found no significant issue. Its
mutable-state reference signatures, AdvanceForNext single-Fail rule and
test-buffer ownership caution are included above. It inspected source/tests
without editing files or executing builds/tests.

## Implementation evidence

Four added cases passed on original source at
`16282d222e5c3157930c453e5b27d35d72ab3590` before extraction:
`DirectionTransitionsPreserveExactChildMoveOrder`,
`FailedDirectionTransitionStopsBeforeEveryLaterChildMove`,
`ReverseOutputsOwnBinaryAndEmptyKeysAcrossReusedChildBuffers`, and
`BeginningExhaustionStopsPositioningAndExplicitSeekRestoresIt`.
The existing reverse-sampling case also asserts exact period counts.
Test-local record/buffer adapters do not alter the shared scripted fixture.

Original and combined implementations pass the same 105 unique cases across
the eight named iterator/compaction/database suites, with exact JUnit parity
and no failures/errors/skips. The merge-direction topic passes eight cases.
The actual unit executable was rebuilt after each topic. The ledger describes
all fifteen new/changed function bodies in this extraction.
FinishReverseScan returns void because it cannot return a status failure;
its parent composes it with the original successful scan completion.

Independent combined review found no significant issue in the seven changed
paths and independently confirmed identical 105 unique cases across the eight
suites, with no failures/errors/skips. It did not execute builds/tests or
independently verify intermediate runs, artifact-to-commit provenance or CI.
