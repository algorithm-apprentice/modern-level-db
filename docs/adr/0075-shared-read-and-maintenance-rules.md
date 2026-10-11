# ADR-0075: Shared Read and Maintenance Rules

- Status: Accepted
- Date: 2026-10-11

## Concept and contract

```text
Concept:
    Share identical decisions without merging different lifetime protocols.
Observed mismatch:
    Public read preparation, level iterator assembly, and background output
    installation each repeat one rule in two consumers.
Current authority:
    ADR-0031, ADR-0035, ADR-0037, ADR-0038, ADR-0057, and ADR-0074.
Invariant that must remain:
    Preserve public results, source order, borrowing, shared retention,
    validation boundaries, lock ownership, durability, and failure cleanup.
```

This is a bounded continuation of
[ADR-0074](0074-database-engine-coordination-structure.md), not a new
coordination architecture. DatabaseEngine remains the sole database-mutex
owner. No persistent bytes, public signatures, scheduling policy, synchronization
mechanism, or resource destruction order change.

## Reviewed candidates

| Candidate | Decision | Reason |
|---|---|---|
| Public snapshot/read option preparation | Implement independently | Validation is identical; ownership is not |
| L0 versus deeper-level iterator assembly | Implement independently | Both consumers implement the same LSM source-composition rule |
| Background output edit installation | Implement independently after bounded design review | Both output-producing paths use the same build-error/closing/install decision |
| SSTable finalization and metadata construction | Defer | Small duplication does not justify a wide helper spanning distinct deletion and directory-sync protocols |
| Separate background/reclamation services | Reject | They would obscure the single mutex and cross-component transition owner |
| A universal unlocked-operation framework | Reject | Reads, writes, metadata I/O, and background exceptions have different failure owners |
| A universal file-overlap algorithm | Reject | Point lookup, flush placement, and L0 compaction closure are not equivalent |

Only the first three candidates are executable slices in this decision.
Each has a separate test-first, implementation, review, and persistence boundary.
Do not start the next implementation slice until the previous one has passed its
focused checks. Remote delivery still follows
[ADR-0006](0006-sequential-pull-request-workflow.md).

## Established mechanisms retained

No new lifetime, locking, or persistence mechanism is proposed. The existing
implementation and its authoritative ADRs are the reference:

- [ADR-0038](0038-public-raii-api.md) and
  [ADR-0057](0057-leveldb-version-output-parity.md) separate borrowed synchronous
  reads from retained child handles.
- [ADR-0031](0031-iterators.md) and
  [ADR-0035](0035-running-compactions.md) merge overlapping L0 files individually
  and concatenate ordered nonzero-level runs.
- [ADR-0037](0037-database-engine.md) and
  [ADR-0074](0074-database-engine-coordination-structure.md) keep output
  protection, sticky failure, publication, and reclamation under one coordinator.

Continue using the current standard-library shared owners, borrowed spans,
unique locks, and Result values. Do not introduce a new RAII cleanup protocol,
runtime validation pass, type-erased callback, or template execution framework.

## Slice 1: Public read preparation

### Read preparation boundary

Replace `PrepareSynchronousRead` and `PrepareRetainedRead` with one private
`PrepareReadOptions` in `src/api/database.cc`.

Its inputs are the borrowed database state, an explicit `has_snapshot` flag,
the borrowed registration pointer, and `fill_cache`. It returns only
`DatabaseEngineReadOptions`; it neither acquires nor retains owners.

### Preserved behavior

1. No Snapshot handle means latest-sequence selection, even though the borrowed
   registration pointer is null.
2. A supplied moved-from Snapshot has a null registration and is InvalidArgument,
   not a request for the latest sequence.
3. A registration from another database is InvalidArgument.
4. The helper reads the sequence only after validation and preserves `fill_cache`.
5. `Get` continues borrowing both state and registration without shared copies.
6. `NewIterator` copies the state and registration owners before preparation,
   then moves the registration into `Iterator::Impl`. Construction failure
   releases local owners normally.
7. Iterator destruction still drops the internal iterator before its snapshot
   and database state.

The owning-result Get overload still delegates to the caller-buffer overload.
Neither moved-from database handling nor public error text changes.

### Read preparation evidence

Add a paired Get/NewIterator snapshot-validation characterization test before
changing production code. Cover no snapshot, a valid snapshot, a moved-from
snapshot, and a foreign snapshot, including unchanged output on API rejection.
Keep existing iterator-retains-snapshot and retained-state tests.

Entry points: `src/api/database.cc`, `src/api/api_internal.h`.
Tests: `tests/unit/api/public_api_native_test.cc`.

## Slice 2: Level iterator assembly

### Iterator assembly boundary

Add an internal `AppendLevelIterators` to `engine/iterators.h` and
`engine/iterators.cc`. Its arguments explicitly identify the retained Version,
level, borrowed file span, table cache, comparator, read options, and child list.

For L0, append a single-file LevelIterator for each supplied file in span order.
For a nonzero level, append one LevelIterator for the entire nonempty span.
An empty span appends nothing. Existing children remain first.

`NewInternalIterator` still adds mutable/immutable children and visits every
Version level. `NewCompactionIterator` still chooses only its two input lists
and supplies `fill_cache=false`.

### Ownership and unsupported use

The helper constructs existing iterator types; it does not copy the file list
or open tables. Children retain the supplied Version, which keeps file metadata
live, but their spans still borrow the caller's list storage.

Ordinary lists live in the retained Version. Compaction lists live in the
unchanged Compaction and must outlive its iterator, as before. Retaining the
Version does not retain an arbitrary external vector.

Use only trusted internal levels and files owned by that Version. Nonzero-level
input runs must be ordered and non-overlapping. Unsupported uses include
temporary external lists, mutated lists, and overlapping deeper-level runs.
Preserve the existing comparator-rvalue rejection for this new borrowed entry
point.

### Iterator assembly evidence

Test the new helper before adding its implementation: empty inputs, preservation
of existing children, per-file L0 versus one deeper run, child order, no I/O
during construction, and retained Version ownership. Keep equal-key tie and
direction-change tests.

Add compaction characterization coverage for selected-input-only iteration,
construction without I/O, forward/reverse traversal, and disabled cache filling.

Entry points: `src/engine/iterators.h`, `src/engine/iterators.cc`,
`src/engine/compaction.cc`.
Tests: `tests/unit/engine/iterators_test.cc`,
`tests/unit/engine/compaction_test.cc`.

## Slice 3: Background output installation

### Background installation boundary

Add a private `DatabaseEngine::ApplyBackgroundEdit` taking a completed
`Result<VersionEdit>`, the owned unique lock, and the existing operation-specific
closing-error text. The error text is materialized only on the closing branch.

The decision order is deliberately exact:

```text
build failed -> return the original build error
otherwise closing -> return the existing Aborted error
otherwise -> VersionSet::LogAndApply under the existing lock protocol
```

The method performs no output release, sticky-error recording, seek-budget
update, notification, immutable reset, or reclamation.

### Preserved transitions

- The caller has reacquired the database mutex before invoking the helper.
- A foreground sticky error arising during unlocked output construction does
  not add a new early exit: a successful already-running flush can still install
  its edit, while cleanup remains forbidden by the sticky error.
- A build error takes precedence over closing.
- `LogAndApply` still prepares topology while locked, unlocks only for metadata
  I/O, and publishes only after durable success.
- Flush still records failure before releasing its output number; success clears
  immutable state, updates seek budgets, wakes writers, then reclaims files.
- Compaction still uses `FinishCompaction`, releases recorded outputs in its
  caller, and leaves trivial moves on their existing direct LogAndApply path.
- Throwing builds or MANIFEST writes still reach BackgroundCall's existing
  exception handler. Pending outputs stay protected on that path.
- Closing may begin after the last compaction entry; a completed output is still
  rejected before MANIFEST installation.

Do not unify flush and compaction completion or move exception handling into
the new helper.

### Background installation evidence

Strengthen failure-characterization tests before implementation: no MANIFEST
append after output construction failure; original returned errors and sticky
errors; retained immutable state on failed flush; released output numbers for
returned failures versus protected numbers after exceptions; no reclamation
after failure. Keep deterministic closing gates, foreground-failure-during-flush,
trivial-move, and flush-during-compaction tests.

Entry points: `src/engine/database.h`,
`src/engine/database_background.cc`, `src/metadata/version_set.cc`.
Tests: `tests/unit/engine/database_background_test.cc`,
`tests/unit/engine/database_read_test.cc`,
`tests/unit/engine/database_write_test.cc`,
`tests/unit/metadata/version_set_test.cc`.

## Verification and completion

Refactors of existing behavior begin with characterization tests that pass
against the original implementation; do not intentionally break production code
to manufacture a red test. A new internal entry point begins with a test that
fails to compile until the entry point is implemented.

Use the existing Windows Debug build with warnings as errors. Run each slice's
focused tests before and after its production change. At the integrated boundary,
run relevant unit tests and the database model/crash/compatibility evidence,
then `python tools/check_documentation.py`.

Record reviewed outcomes here only after validation. Update the architecture,
learning source tour, and ADR index where responsibilities or names moved.
Remote CI, non-Windows sanitizers, and coverage remain separate delivery gates;
local Windows checks must not be described as having run them.

## Design review

A bounded independent static review on 2026-10-11 examined the three proposed
extractions against their current sources and failure/lifetime contracts.
It identified no actionable design findings. Implementation remains gated by
the test and review boundaries above.

## Implementation outcomes

### Read preparation outcome

Slice 1 shares `PrepareReadOptions` and removes the owning PreparedRead wrapper.
The snapshot-validation matrix passed against the original implementation before
the extraction. It covers both Get result shapes and NewIterator, both cache
settings, absent/valid/moved-from/foreign snapshots, and unchanged caller output
on API rejection. Synchronous reads still borrow; iterators still retain.
