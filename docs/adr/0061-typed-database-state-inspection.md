# ADR-0061: Typed Database State Inspection

## Status

Implemented by PR #92 after design PR #91.

Implementation and final validation are complete. Debug, Release,
compatibility, model, crash, ASan/UBSan, TSan, fuzz smoke, benchmark,
profiling-contract, and changed-code coverage evidence passed. A bounded
GPT-5.6 Sol implementation review found no actionable issue.

## Context

Modern LevelDB now implements and verifies the complete initial LSM pipeline:
writes, snapshots, point reads, iteration, recovery, flushes, size and seek
compactions, obsolete-file cleanup, compression, compatibility, crash
consistency, and pinned LevelDB read/write-path parity.

The public API deliberately exposes operations rather than internal engine
objects. That keeps ownership and persistence policy private, but it also
makes several important storage-engine transitions invisible to a learner or
an application diagnosing its own workload:

- how many files and bytes currently occupy each level;
- which sequence boundary is published;
- whether explicit snapshots are retaining old history;
- whether writers are queued;
- how much arena storage the mutable and immutable memtables reserve;
- whether background work is scheduled and which output file numbers remain
  protected from cleanup; and
- whether a sticky write/background failure has stopped later writes and
  maintenance.

The guided learning path added by PR #90 identifies a typed state snapshot
as the highest-priority learning-oriented extension.
It provides a concrete current caller: examples and experiments that explain
why writes slow down, why disk usage does not immediately fall after deletes,
and why old history or files remain live.

This is not a request for a general metrics framework. Detailed read-path
counters already exist in the separately built profiling diagnostics. The
production engine has no current caller for subscriptions, exporters,
histograms, per-operation latency, arbitrary property strings, or a plugin
system.

## Prior art

### Pinned Google LevelDB

The pinned LevelDB revision
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5` exposes:

```cpp
virtual bool GetProperty(const Slice& property, std::string* value) = 0;
```

Its recognized string properties include:

- `leveldb.num-files-at-level<N>`;
- `leveldb.stats`;
- `leveldb.sstables`; and
- `leveldb.approximate-memory-usage`.

`DBImpl::GetProperty` takes the database mutex and formats text. Its memory
property adds block-cache charge and mutable/immutable memtable arena usage.
Pinned LevelDB's arena publishes its usage through
`std::atomic<size_t>` with relaxed operations.

Modern adopts the useful observation boundary but not the stringly typed
interface. A structured snapshot makes field presence, numeric units, and
ownership explicit, avoids property-name parsing, and can evolve through
normal reviewed API changes while the project is pre-alpha.

### Modern read diagnostics

`instrumentation/read_diagnostics.h` supplies compile-time-enabled,
foreground-thread
sessions for profiling table/cache/read stages. Those counters are sampled
instrumentation with a separate diagnostic binary. They are not a stable
database-wide production state API and must not be enabled on every hot path
to implement this feature.

## Decision

Add one synchronous, on-demand, typed snapshot:

```cpp
// include/modern_leveldb/database_state.h

inline constexpr std::size_t DatabaseLevelCount = 7;

struct DatabaseLevelState {
  std::size_t file_count = 0;
  std::uint64_t file_bytes = 0;
};

struct DatabaseState {
  std::array<DatabaseLevelState, DatabaseLevelCount> levels{};

  std::uint64_t last_sequence = 0;
  std::size_t snapshot_count = 0;
  std::optional<std::uint64_t> oldest_snapshot_sequence;

  std::size_t write_queue_depth = 0;
  std::size_t mutable_memtable_bytes = 0;
  std::optional<std::size_t> immutable_memtable_bytes;

  std::size_t protected_output_count = 0;
  bool background_work_scheduled = false;
  std::optional<Error> sticky_error;
};
```

Add to `Database`:

```cpp
[[nodiscard]] Result<DatabaseState> GetState();
```

`db.h` includes `database_state.h`, so callers of the main facade need no
internal header.

The returned value owns every field and remains valid after later database
activity or destruction. It contains no file names, keys, byte views,
pointers, callbacks, or handles into the engine.

### Field contracts

| Field | Contract |
|---|---|
| `levels[level].file_count` | Number of files in the current Version at that level |
| `levels[level].file_bytes` | Sum of the current Version's recorded file sizes at that level |
| `last_sequence` | Last sequence published to latest reads |
| `snapshot_count` | Number of live explicit snapshot registrations retained by Snapshot handles or iterators created from them, including duplicates at one sequence |
| `oldest_snapshot_sequence` | Smallest registered explicit snapshot sequence, or no value when none exist |
| `write_queue_depth` | Writers currently in the serialized write queue, including its active leader |
| `mutable_memtable_bytes` | Arena-reserved bytes published at the same last completed successful write boundary as `last_sequence` |
| `immutable_memtable_bytes` | Arena-reserved bytes of the immutable memtable, when one occupies the flush slot |
| `protected_output_count` | Exact cardinality of the engine's set of fresh table numbers protected from obsolete-file cleanup |
| `background_work_scheduled` | Whether an accepted background engine task is queued or running |
| `sticky_error` | Exact copy of the engine's first recorded sticky error, when present |

The sequence values are operation-order identifiers, not timestamps.
The snapshot fields cover explicit `GetSnapshot` registrations. Iterators
also retain their concrete read sources, but an iterator without an explicit
Snapshot does not create a registration in the snapshot multiset.

`file_bytes` describes recorded SSTable file sizes, not live user-value
bytes, WAL bytes, MANIFEST bytes, total directory usage, compressed logical
size, or write amplification.

Memtable byte fields describe arena-reserved blocks and their owner-pointer
accounting. They are not exact payload bytes or a process-wide memory limit.
The immutable field describes the single flush slot; other source lifetimes
may remain retained by active readers or iterators and are intentionally not
claimed by this first API.

`background_work_scheduled` does not distinguish queued from running work,
flush from compaction, percentage complete, or remaining duration.
`protected_output_count` is a cleanup-protection invariant, not an activity
or completed-work counter. A protected number normally belongs to an active
flush or compaction, but it may remain after an exceptional background
termination. In that state `background_work_scheduled` is false,
`sticky_error` is present, and cleanup remains stopped because installation
may be uncertain.

### Layer boundary

The engine must not include a public API header.

Add a corresponding internal `DatabaseEngineState` value in the engine
layer. `DatabaseEngine::GetState` captures it. The API layer converts it to
the public `DatabaseState` and contains:

```cpp
static_assert(DatabaseLevelCount == NumLevels);
```

This preserves the dependency rule that `api` depends on `engine`, while
`engine` never depends on `api`.

### Synchronization and snapshot consistency

`DatabaseEngine::GetState` takes the database mutex, copies its state, and
performs no filesystem I/O, waiting, cache lookup, table open, or background
drain.

While that mutex is held:

- the current Version and its file vectors are stable;
- snapshot registrations, queue membership, protected outputs, scheduling
  state, and the sticky error are stable;
- the immutable memtable is no longer mutated; and
- the state copy cannot interleave with sequence publication or topology
  installation.

The operation is O(number of current table files) because it sums each
level's recorded file sizes. It does not enumerate or return individual
files.

The byte sum uses checked addition. If a level's recorded file sizes cannot
be represented by `std::uint64_t`, `GetState` returns `Aborted` rather than
wrapping or returning a success-shaped value.

### Mutable memtable publication invariant

Holding the database mutex alone does **not** make the current arena's
`memory_usage()` safe to read.

The write-queue leader deliberately releases that mutex while
`CommitGroup` appends the WAL and inserts the batch into the mutable
memtable. `Arena::memory_usage_` is currently an ordinary `std::size_t`
updated when insertion allocates a block. A state query may acquire the
database mutex during that unlocked commit.

Do not solve the public observation API by racing on the arena counter.
Do not make every arena user atomic merely because pinned LevelDB does.
Modern can preserve a stronger successful-commit cross-field boundary with a
narrower mechanism:

```cpp
std::size_t published_mutable_memtable_bytes_ = 0;
```

This field is protected by the database mutex and follows these rules:

1. Initialize it after recovery creates the first mutable memtable.
2. Reset it to the new mutable memtable's usage when rotation installs a new
   WAL/memtable pair.
3. After `CommitGroup` returns successfully and the leader reacquires the
   mutex, refresh it from the still-exclusively-owned mutable memtable
   immediately before publishing the group's final sequence.
4. If commit throws, record the sticky error and rethrow without publishing
   either the group's sequence or a new mutable-memory boundary. A rare
   allocation exception may have inserted part of the batch or reserved
   arena storage, but those sequences remain above `last_sequence`, later
   writes are stopped, and the state deliberately retains the last completed
   published pair.
5. A returned current-WAL append or sync failure is sticky and inserts no
   memtable entries, so it also retains the previous pair.

Only the queue leader mutates the current memtable, so once it has finished
or unwound, reading `memory_usage()` to publish the value has no concurrent
writer.

During an in-flight commit, `GetState` therefore reports the previous
published `last_sequence` and its corresponding published mutable-memory
boundary. It may also report the active writer in `write_queue_depth`.
The same is true after an exceptional partially inserted batch: the published
pair remains the last completed boundary, while `sticky_error` reports that
the engine stopped. The byte field can then be lower than actual reserved
arena storage and does not claim to describe unpublished failure debris.

The existing room-for-write path continues reading the live arena usage while
the queue leader owns the write path. The published value is for observation,
not for rotation policy.

### Failure and lifetime behavior

Calling `GetState` on a moved-from `Database` returns `InvalidArgument`, as
other facade methods do.

An existing `sticky_error` does not make state inspection fail. The query
succeeds and returns a copy so callers can inspect why later writes,
background scheduling, and obsolete-file cleanup stopped. Reads and snapshot
registration remain available under their existing contracts.

Current-WAL append and sync failures are sticky. New-WAL open or
pre-installation directory-sync failures during rotation return to their
writer without installing new state and remain retryable. If closing the old
WAL **returns an error** after the replacement objects and durability barriers
are ready, the error becomes sticky and rotation still installs the new WAL,
immutable memtable, and mutable memtable before later writes observe the
error.

`sticky_error` is state, not an exception-history log. An exception that
propagates before current engine error containment records it can leave this
field empty. In particular, a thrown old-WAL close currently unwinds rotation
before replacement installation; this observation API does not change that
existing exception behavior. A broader rotation-exception hardening change
requires its own design and tests.

The API reports no I/O error because it performs no I/O. Normal allocation
failure while copying an error message follows the project's ordinary C++
allocation behavior.

The same public-object concurrency rule applies: database methods may run
from several threads, but the `Database` object itself must not be moved or
destroyed concurrently with `GetState`.

## Public example

```cpp
auto state = database.GetState();
if (!state.has_value()) {
  return state.error();
}

const auto& level_zero = state->levels[0];
std::cout << "L0 files: " << level_zero.file_count << '\n';
std::cout << "mutable memtable bytes: "
          << state->mutable_memtable_bytes << '\n';
```

The API intentionally returns numbers rather than preformatted prose.
Learning documents and applications decide how to render them.

## Explicitly deferred behavior

- String property names or compatibility with LevelDB's `GetProperty`.
- Individual SSTable names, key ranges, file numbers, or debug dumps.
- WAL, MANIFEST, block, or table decoding; those belong to a later offline
  inspector.
- Block-cache charge, table-cache membership, operating-system page-cache
  state, and process RSS.
- Retained old-Version or iterator-held memtable counts without complete
  ownership tracking.
- Completed flush/compaction counters, bytes read/written by compaction,
  timings, histograms, rates, or latency percentiles.
- A distinction between queued and running background work or between flush
  and compaction.
- Push subscriptions, callbacks, event streams, logging, metrics exporters,
  or asynchronous queries.
- Public Flush, CompactRange, repair, backup, or configuration knobs.

Each deferred item requires a concrete caller and its own synchronization,
ownership, cost, and failure contract.

## Sequential delivery roadmap

### PR 1: design only

- Add this ADR.
- Obtain one bounded GPT-5.6 Sol design review.
- Resolve every justified finding before merge.
- Run the documentation-only CI acknowledgements from ADR-0048.

No production or test code is changed in this PR.

### PR 2: implementation and learning example

Implement the complete first version in one coherent PR:

1. Add the internal engine-state value and checked per-level byte
   accumulation.
2. Add the mutex-protected published mutable-memory field and wire
   initialization, rotation, and successful sequence publication while
   preserving the previous boundary on commit exceptions and returned
   WAL/sync failures.
3. Add `DatabaseEngine::GetState`.
4. Add the public header, facade conversion, and moved-from handling.
5. Add focused engine and public API tests.
6. Update README/API documentation and add one state-inspection exercise to
   the learning path.

Do not begin an offline inspector, manual flush, manual compaction, or another
feature in parallel.

## Validation plan

Development remains test-first.

### Focused engine tests

- A new database reports seven empty levels, sequence zero, no snapshots,
  no queued writer, no immutable memtable, no pending output, no scheduled
  work, no error, and a nonzero mutable arena reservation.
- A large write that exhausts the current arena block and requires a
  dedicated allocation advances the published sequence and mutable-memory
  boundary; a small write may advance only the sequence because arena
  accounting measures reserved blocks.
- An empty batch advances neither the published sequence nor memory boundary.
- Multiple explicit snapshots report their count and oldest sequence and
  disappear after release.
- A pre-populated memtable, a value larger than both the remaining normal
  arena block and the dedicated-allocation threshold, and
  `BlockingComparator` pause a commit during skip-list insertion after entry
  allocation while the database mutex is released.
  `GetState` still reports the previous sequence/memory boundary and an active
  write queue; after release, both published fields advance and the queue
  empties. This test would expose an implementation that reads the live arena
  counter.
- A thrown commit retains the previous published pair and reports the sticky
  error. Allocation-failure debris is covered by the documented invariant;
  ordinary unit tests do not require a multi-gibibyte or allocator-global
  failure mechanism.
- Rotation reports an immutable memtable and accepted background work.
- A background flush blocked after reserving an output reports one protected
  output; after installation, the count clears and exactly one level reports
  the installed file.
- An exception after output reservation leaves the protected count nonzero,
  clears the scheduled-work flag, and reports the sticky error.
- A scheduling or background failure is copied into the state while the
  state query itself succeeds.
- Checked level-byte accumulation rejects overflow without wrapping.

Existing `MemoryFileSystem` operation hooks, `Gate`, and `ManualExecutor`
provide deterministic seams; no sleeps are added to the new tests.

### Public API tests

- The public type is copyable/movable as an owning value.
- The facade maps every engine field.
- The state remains valid after the database changes or is destroyed.
- A moved-from database rejects the query.

### Required gates

Run the smallest focused tests during development, then:

1. complete Debug unit and CMake consumer tests;
2. Release with warnings as errors;
3. compatibility, ordered-map model, and crash tiers;
4. ASan/UBSan and TSan;
5. GCC 13 changed-code coverage;
6. benchmark and profiling contract smoke tests, without collecting a new
   performance result; and
7. one bounded GPT-5.6 Sol implementation review.

The state query is not a benchmark feature. No performance claim or frozen
measurement matrix is required unless profiling identifies a material cost
in existing workloads.

## Acceptance criteria

The feature is complete when:

- every public field has the exact semantics above;
- no engine layer includes a public API header;
- state inspection performs no I/O and never waits for background work;
- mutable-memory observation is race-free and aligned with sequence
  publication at completed successful commit boundaries;
- an in-flight commit cannot produce a success-shaped partially published
  state;
- an exceptional partial insertion retains the previous published pair and
  exposes the sticky error instead of claiming exact current arena usage;
- sticky errors remain inspectable;
- tests deterministically cover the synchronization transitions; and
- all required gates and bounded review pass.

## Consequences

### Positive

- Learners can observe LSM topology, MVCC retention, write pressure, and
  maintenance state through one stable value.
- Applications gain basic diagnosis without parsing implementation-specific
  strings.
- The memory-publication invariant is explicit and testable.
- The API adds no persistent format, I/O, worker, or hot-path counter.

### Negative

- The public API gains another type whose field semantics must remain
  documented.
- Per-level byte totals require an O(current files) mutex-held scan.
- The first version intentionally omits cache and historical-source memory,
  so it is not total process memory.
- A later richer statistics API may require new counters rather than merely
  extending this snapshot.

## Rejected alternatives

### Copy LevelDB's `GetProperty`

Rejected. String names and formatted values hide types, units, optionality,
and error handling. Modern has no source-compatibility requirement.

### Return JSON or a formatted report

Rejected. Formatting is a caller concern, and a text schema would become
another compatibility surface.

### Read `MemTable::memory_usage()` under the database mutex

Rejected. The queue leader mutates the arena while that mutex is released.
This would introduce a data race in the observation feature.

### Make the arena counter atomic

Rejected for this task. It would make an individual number race-free but
could expose memory from an in-flight unpublished batch beside the previous
`last_sequence`. The published field provides the narrower requirement and a
stronger successful-commit cross-field boundary.

### Hold the database mutex throughout every write commit

Rejected. It would regress the completed write-path parity design and block
concurrent reads/state capture on WAL I/O and memtable insertion.

### Track every useful metric now

Rejected. Per-operation counters, cache statistics, compaction accounting,
timers, and exporters add hot-path cost and lifecycle questions without a
current caller.

### Build the offline inspector first

Rejected as the immediate next task. An inspector remains valuable, but it
teaches persistent bytes rather than live queue, snapshot, and maintenance
transitions. It is a separate future feature.

## References

- [Modern public database API](../../include/modern_leveldb/db.h)
- [Modern database engine](../../src/engine/database.h)
- [Modern write commit](../../src/engine/database.cc)
- [Modern arena accounting](../../src/memory/arena.h)
- [Modern read diagnostics](../../src/instrumentation/read_diagnostics.h)
- [Need-driven simplicity](0010-need-driven-simplicity.md)
- [Public RAII API](0038-public-raii-api.md)
- [Completed read-path parity](0053-leveldb-read-path-parity.md)
- [Completed write-path parity](0060-leveldb-write-path-parity.md)
- [Learning-oriented feature comparison](../learning/11-leveldb-comparison.md)
- [Pinned LevelDB DB API](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/db.h)
- [Pinned LevelDB property implementation](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [Pinned LevelDB arena accounting](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/arena.h)
