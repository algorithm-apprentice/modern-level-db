# ADR-0060: LevelDB Write-Path Parity

## Status

Accepted by [#82](https://github.com/algorithm-apprentice/modern-level-db/pull/82).
Implementation proceeds through the sequential milestones below. A bounded
GPT-5.6 Sol design review found four issues, all resolved before acceptance:
preserve LevelDB's equivalent table directory barrier, make exclusive batch
mutation opt-in, include runtime VersionBuilder parity, and extend the actual
retained benchmark runner with raw per-iteration data.

Implementation progress: Milestones 1 and 2 are complete; Milestones 3 through
5 remain pending and sequential. No write-path performance measurements or
profiles have been collected for the partial implementation.

## Context

Modern LevelDB implements the complete LevelDB write pipeline and intentionally
adds typed errors, exception containment, stronger WAL-creation durability,
and defensive validation. The resulting behavior is correct, but
the ordinary successful path still differs from pinned Google LevelDB revision
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5` in several avoidable ways:

- a batch header and every appended record can allocate separately;
- the uncontended writer copies its batch, dispatches through `std::function`,
  and allocates shared result ownership;
- WAL fragmentation materializes a vector and recomputes record-type CRCs;
- owned batches, memtable entries, internal keys, and table keys are repeatedly
  revalidated after their invariants are already established;
- skip-list nodes use two arena allocations and different alignment and
  accounting;
- foreground writers retain the database mutex during MANIFEST append and
  sync.

These costs cannot be justified collectively as "more safety." Safety checks
belong at an explicit boundary. Internal paths may trust a boundary only when
the invariant, ownership, lifetime, invalidation events, and checked fallback
are documented and tested.

[ADR-0053](0053-leveldb-read-path-parity.md) established the corresponding
read-path rule: complete a pinned-mechanism parity pass before accepting or
rejecting isolated optimizations. This ADR applies that rule to writes.

## Decision

Align the ordinary successful write pipeline with pinned LevelDB from public
batch encoding through version installation and obsolete-file cleanup.

Parity means:

1. use the pinned mechanism when Modern has no stronger requirement;
2. use an equivalent or cheaper mechanism when it preserves the same externally
   observable contract;
3. retain stronger behavior only as a named safety or durability exception;
4. keep external-data validation at API, recovery, decoded-format, and output
   format boundaries;
5. use assertions, not repeated recoverable checks, after an internal invariant
   is established;
6. measure only the completed pipeline, never an isolated intermediate
   milestone.

This ADR does not require source-level imitation where Modern's ownership model
already provides an equal or cheaper invariant. It does require every
normal-success difference to be classified rather than implicitly excused.

## Parity boundary

The audited boundary starts at:

- `Database::Put`, `Database::Delete`, `Database::Write`, and
  `Database::WriteExclusive`;
- `WriteBatch::Put`, `Delete`, `Append`, and `Clear`.

It ends after:

- writer completion and sequence publication;
- mutable-to-immutable rotation;
- WAL append and optional sync;
- memtable insertion;
- immutable flush and compaction table construction;
- MANIFEST append, sync, and in-memory version installation;
- background error propagation, shutdown, and obsolete-file cleanup.

Reads performed by the `mixed50` workload are covered by
[ADR-0053](0053-leveldb-read-path-parity.md); this ADR changes them only where
write-path ownership or scheduling necessarily interacts with them.

## Reference configurations

The source reference remains the pinned LevelDB revision above. Write checksums
are material enough that the primary parity gate must not compare Modern's
hardware-dispatched Google CRC32C with LevelDB's portable checksum
implementation.

The final matrix therefore has three explicit roles:

| Role | CRC32C | WAL-creation durability | Purpose |
|---|---|---|---|
| Modern matched | pinned Google CRC32C with runtime ARM64 dispatch | LevelDB-equivalent control | primary candidate |
| pinned LevelDB hardware | the [ADR-0059](0059-leveldb-hardware-crc32c-control.md) measurement-only link patch | pinned behavior | primary reference |
| Modern production | pinned Google CRC32C with runtime ARM64 dispatch | file-and-directory WAL creation barrier | production regression control |

The canonical `HAVE_CRC32C=0` LevelDB binary remains unchanged. A separate
descriptive control measures hardware LevelDB against that canonical binary so
the primary reference change is attributable. This ADR is the separate
decision, required by ADR-0059, that makes the hardware-CRC variant the primary
reference for write-path parity only.

The hardware variant must continue to satisfy ADR-0059's source, link,
provenance, binary-hash, runtime-dispatch, and compatibility requirements. It
does not become a production dependency.

For `writebatch`, the primary Modern role explicitly selects
`WriteExclusive`. Existing benchmark invocations retain the const-copying
`Write` path unless a role selects otherwise.

## Complete invariant and divergence audit

The table records where every trusted invariant is established, what protects
it, what can invalidate it, which ordinary release work can be removed, and how
the difference is classified.

| # | Layer or function | Pinned mechanism | Current Modern mechanism | Invariant establishment | Protection and lifetime | Invalidation events | Release work removed | Decision |
|---:|---|---|---|---|---|---|---|---|
| 1 | Public Put/Delete/Write and batch encoding | `std::string` batch bytes; direct record append; mutable `WriteBatch*` | heap PImpl over `vector<byte>`; temporary record vectors; const write | public batch mutation checks count and representable key/value lengths; raw WAL bytes use checked decode | private bytes and an explicit exclusive batch borrow | moved-from object, caller access during an exclusive write, or external encoded bytes | header allocation, temporary-record allocation, ordinary leader copy | use string/inline-header storage and direct reserved append; add an explicitly named `WriteExclusive` fast path while preserving existing `Write(const WriteBatch&)` behavior |
| 2 | Writer layout, enqueue, and wakeup | stack writer and direct DB calls | stack writer, stored `std::function`, shared result for every group | caller owns the DB mutex before enqueue and keeps its stack writer alive through completion | DB mutex, deque order, condition variable, leader guard | reentrancy, missing lock, or callback violating queue ownership | type erasure and uncontended shared-result allocation | template callback dispatch; allocate shared result only before a real multi-writer commit; retain preallocated abort state |
| 3 | Group compatibility and size | leader batch is direct until a follower requires scratch; sync and 1 MiB/+128 KiB limits | leader is always copied; same arithmetic; forced writer runs alone | queued batches are immutable while queued and the leader defines sync compatibility | DB mutex and queue-front leadership | caller mutation or queue reentrancy | complete single-writer batch copy | use the leader directly and switch to reusable scratch only for a real follower; retain forced-alone as an internal control exception |
| 4 | Sequence assignment and overflow | hidden sequence set once; arithmetic largely unchecked | full key scan, checked reader, repeated range checks, success-only publication | API/recovery validate entry shapes; the leader reserves one contiguous sequence range | DB mutex, private batch bytes, `VersionSet::last_sequence` | exhaustion, corrupt external batch, or sequence reuse | precommit scan and per-entry range checks | keep explicit 56-bit/count errors, set sequence once, trust owned iteration, and restore caller-visible hidden state |
| 5 | `MakeRoomForWrite` | raw current version, one delay, room/immutable/L0 loop | shared current-version copy and different arena accounting | only the queue front enters; topology and memory use are mutex-protected | DB mutex, background condition, one background task | topology mutation outside the mutex or wrong accounting | current-version reference-count traffic | use `current_raw()` under the mutex and match arena accounting; retain trigger order |
| 6 | Log/memtable/version ownership while unlocked | queue front protects log/mem; background pins immutable/base | equivalent RAII pins, but ownership contract is implicit | prepare completes before unlock; rotation cannot occur behind the front writer | queue leadership, DB mutex transitions, shared background pins | reentrant write, inline scheduler execution, or premature pin release | no new check; permits borrowed access without per-write ownership copies | document and assert the existing ownership contract; retain RAII pins |
| 7 | WAL fragmentation and buffering | streaming fragments and precomputed type CRCs; flush per fragment | fragment vector and repeated type CRC; one flush per logical record | logical record is stable and one writer owns `block_offset` | front-writer serialization and poisoned first-error state | concurrent `AddRecord`, record mutation, or use after I/O error | fragment allocation and repeated one-byte CRC setup | add an allocation-free cursor and precomputed type CRCs; retain the cheaper logical-record flush |
| 8 | Sync and durable errors | leader sync controls group; sync failure becomes permanent | same sync rule; every uncertain commit failure or exception is sticky | append precedes sync and memtable publication | queue front, WAL first error, DB background error | ignored I/O status or later writes after uncertainty | no material normal-success work beyond predictable branches | retain broader sticky handling as a failure-path safety exception |
| 9 | Memtable, comparator, skip list, and arena | one decode pass; unchecked pack/insert; trusted trailers; one node allocation | checked reopen and add; defensive compare; release duplicate check; two allocations | owned API batches and checked recovery establish tags, lengths, key size, and sequence uniqueness | private bytes, monotonic sequence allocation, single writer, immutable arena storage | corrupt WAL, sequence reuse, or unsupported external file mutation | validation scan, repeated entry checks, defensive decode, duplicate compare, extra allocation | add trusted owned iteration/insertion/comparison; combine node and links; match 8-byte alignment and per-block pointer accounting |
| 10 | Last-sequence publication | publishes locally advanced sequence after the attempt, including failure | publishes only after WAL and memtable success | a visible sequence has corresponding memtable entries; all commit failures are sticky | DB mutex and monotonic `SetLastSequence` | publication before insert or allowing later writes after failure | no success-path difference | retain success-only publication as a coupled failure-semantics exception |
| 11 | WAL/memtable rotation | create new WAL and install without a directory barrier; failed number may be reused | preallocate state, sync WAL creation, never reuse failed number, then install | only the queue front rotates and all replacement objects exist before state change | DB mutex, queue front, directory lock, RAII temporaries | create/sync/close failure or external directory mutation | one low-frequency file/directory durability barrier | retain preallocation and no reuse; route WAL creation through the matched WAL-creation control while keeping the durable default |
| 12 | Flush scheduling and wakeup | infallible schedule; wake at task completion | fallible injected scheduler; wake when immutable drops and again at task end | scheduler queues without running inline; one background task is marked scheduled | DB mutex, scheduled flag, condition variable | inline execution, accepted task loss, or early flag clear | no ordinary write cost beyond failure branches; early wake is cheaper | retain scheduler failure handling and early immutable wake as explicit exceptions |
| 13 | Table build, version delta, and MANIFEST install | table order and version overlap are debug assertions; Builder tracks edit additions/deletions and linearly merges ordered files; MANIFEST `Sync()` supplies the directory barrier and releases the DB mutex | repeated table-key checks; VersionBuilder copies all files into maps, allocates nodes, copies/sorts them back, and validates overlap in release; a pre-MANIFEST table-directory sync supplies the equivalent barrier; MANIFEST I/O holds the DB mutex | iterators yield valid ordered internal keys; installed versions are sorted and non-overlapping above L0; internal edits contain valid file metadata; edit and candidate version are staged first | iterator/version pins, pending outputs, single manifest writer, DB lock before/after I/O | malformed external input, invalid internal edit, concurrent manifest writer, edit mutation, or premature output deletion | repeated key/topology validation, map-node allocation and sorting, and foreground lock hold during MANIFEST I/O | add trusted table and runtime version-delta paths, retain checked recovery/output boundaries and exactly one pre-durable-MANIFEST directory barrier, and make unlock/relock exception-safe |
| 14 | Background error propagation | first error is sticky and stops writes, scheduling, and cleanup | same, plus all commit/scheduler/exception failures | uncertain durable state permits no later mutation or deletion | DB mutex and first-error ownership | overwriting the first error or cleanup after uncertainty | failure-only differences | retain the broader sticky set; never return a success-shaped fallback |
| 15 | Shutdown and exception unwind | wait for background; recheck shutdown after flush build; release WAL later | close WAL, then wait; compaction checks closing; flush lacks the post-build check; exceptions are contained | destruction is not concurrent with public calls; every unlocked operation reacquires before state access | atomic closing flag, DB mutex/condition, queue and background guards | close during unlocked flush, lost lock on exception, or cancelled accepted task | exception containment is failure-only | add the missing flush check; retain exception-safe guards and early WAL close as explicit exceptions |
| 16 | Obsolete-file cleanup | preserve live/pending/log/MANIFEST files; delete unlocked; skip after error | same, with `prev_log_number == 0`, unknown-file retention, and earlier writer wake | every possibly live output is installed or in `pending_outputs`; errors freeze cleanup | DB mutex for live-set creation, version pins, pending outputs | removing pending protection before a certain install result or external modification | no ordinary write-path difference | retain cleanup; strengthen invariant tests and report residual background debt without forced quiescence |

## Trusted invariant registry

### I1. Owned batch validity

An `EncodedWriteBatch` created through public `WriteBatch` operations has:

- a complete 12-byte header;
- a count equal to the number of encoded records;
- only value and deletion record kinds;
- varint lengths wholly contained in private storage;
- user keys accepted by the internal-key and memtable encodings;
- values and complete entries representable by Modern's 32-bit length fields.

`Put`, `Delete`, `Append`, copy, and `Clear` preserve this invariant or leave the
object unchanged on error. Direct append must detect a key or value view that
aliases current batch storage; only that rare path may stage bytes before a
reserve or resize invalidates the view.

External WAL bytes do not receive I1. They continue through
`WriteBatchReader::Open` and recovery validation.

### I2. Queued batch stability

From enqueue until completion:

- the writer object remains on its caller's stack;
- a `WriteExclusive` batch is exclusively borrowed by that call;
- followers may be read into group scratch but are not mutated;
- only the queue front performs prepare or commit.

`WriteExclusive` documents that the batch must not be read, copied, mutated, or
submitted anywhere else until the call returns. Existing calls to
`Write(const WriteBatch&)` retain their const behavior, source compatibility,
and suitability for a shared immutable batch.

### I3. Group validity

The group is either:

- the leader's mutable batch, with no follower; or
- reusable scratch containing an exact append of the leader and every admitted
  follower.

The queue mutex fixes membership, order, sync compatibility, and the last
writer. The group count and size limits are established before sequence
assignment.

### I4. Sequence ownership

While the front writer commits:

- `[first_sequence, first_sequence + count)` is within the 56-bit sequence
  space;
- no other writer can publish or reuse that interval;
- each trusted entry derives its sequence from the group start and ordinal;
- the caller's hidden batch sequence is restored on every status and exception
  path.

An empty group does not advance `last_sequence`.

### I5. Foreground log and memtable ownership

After prepare releases the DB mutex, no other writer can rotate or replace the
borrowed WAL and mutable memtable because the committing writer remains at the
queue front. Background work uses only the immutable memtable, pinned versions,
and its own output files.

### I6. Trusted memtable keys

Trusted insertion receives an I1 batch plus I4 sequences. It constructs an
internal key with a known eight-byte trailer and a length-prefixed memtable
entry whose lengths are representable. Sequence uniqueness makes duplicate
internal keys impossible in one memtable.

`MemTable::AddTrusted`, `SkipList::InsertTrusted`, and
`InternalKeyComparator::CompareTrusted` may assert these properties. Checked
entry points remain for recovery and direct unit tests.

### I7. Arena node layout

The skip-list allocation contains one `Node` followed by individually
constructed trailing atomic links. `LinkAt(level)` is used only for levels
below the height allocated by `NewNode`. The arena alignment is
`max(sizeof(void*), 8)`, matching pinned LevelDB and satisfying every current
arena-resident type.

Memory usage adds the allocated block size plus one block-owner pointer per
block, matching pinned rotation accounting.

### I8. Ordered table input

Flush input comes from a trusted memtable. Compaction input has already passed
block decoding and internal-key validation. Both yield strictly increasing
internal keys according to the configured comparator.

`TableBuilder` may use a trusted key/order path. `BlockBuilder` retains its
recoverable 32-bit output-size guard because compatible tables are opened
lazily and may not have originated from Modern's API boundary.

### I9. Trusted runtime version deltas

Every installed `Version` is sorted by smallest internal key within each level,
and levels above L0 are non-overlapping. Flush and compaction generate internal
`VersionEdit` objects whose file metadata, level placement, deletions, and
additions have already been checked by their builders.

Runtime `LogAndApply` may therefore use pinned LevelDB's delta mechanism:

- collect only deleted file numbers and added file metadata per level;
- order the small added-file set by smallest internal key and file number;
- linearly merge it with the already ordered current version;
- use debug assertions for duplicate files and level-overlap invariants.

MANIFEST recovery and tests that accept decoded or deliberately malformed edits
retain the checked map/sort/validation builder.

### I10. Manifest writer serialization

Exactly one background task calls runtime `VersionSet::LogAndApply`. Before
unlocking it:

- validates the edit;
- builds and finalizes the candidate version;
- captures log, previous-log, next-file, and last-sequence fields for the
  durable edit;
- constructs the shared candidate version;
- reserves any container capacity needed for non-throwing installation;
- captures compact-pointer updates.

Foreground writers may allocate later file numbers and publish later sequences
while MANIFEST I/O is in progress. Installation must not roll those global
counters back. It installs only the staged version, log numbers, and compact
pointers represented by the durable edit.

The owning `std::unique_lock<std::mutex>` is reacquired on every return and
exception path.

Modern's table-directory sync remains before `LogAndApply`. Pinned LevelDB
performs the same one-directory-sync guarantee from MANIFEST `Sync()` before
the MANIFEST itself becomes durable. Moving or removing that barrier is not
part of the matched control.

### I11. Pending-output protection

An output file number remains in `pending_outputs` until its MANIFEST result is
known under the DB mutex. Cleanup cannot remove a live, staged, or uncertain
output. Any uncertain MANIFEST result becomes sticky before pending protection
is removed.

## Accepted divergences

| Divergence | Classification | Reason |
|---|---|---|
| `Database::Write` copies before using the exclusive fast path | preserved API contract | existing immutable/shared batch use remains const and source-compatible |
| every uncertain commit failure is sticky | safety exception | later writes cannot safely distinguish fully absent from partially durable state |
| last sequence advances only after successful memtable insertion | failure-semantics exception | sticky failures make sequence gaps unnecessary and readers never observe an unmaterialized sequence |
| one `WritableFile::Flush` per logical WAL record | cheaper equivalent | normal records have one fragment; large records avoid repeated user-buffer flushes without changing ordering |
| new file numbers are not reused after failed creation | safety exception | avoids identity ambiguity and has no normal-success cost |
| scheduler rejection/exception is reported | safety and testability exception | injected executors are fallible; silent scheduling loss can deadlock writers |
| immutable waiters wake before obsolete-file deletion | cheaper equivalent | the required room condition is already true; cleanup remains serialized |
| unknown database files are retained | safety exception | cleanup must not delete unsupported external content |
| WAL closes before the destructor waits for background work | shutdown exception | background flush/compaction does not use the current WAL; public calls concurrent with destruction remain unsupported |
| table-directory sync occurs before `LogAndApply` rather than inside MANIFEST `Sync()` | equivalent mechanism | both engines perform one directory barrier before an edit that references the new table becomes durable |
| production WAL creation syncs file and directory state | stronger durability default | the matched benchmark control disables it explicitly rather than weakening production |

## Public API decisions

### Explicit exclusive-batch write

Add:

```cpp
[[nodiscard]] Status WriteExclusive(WriteBatch& batch,
                                    const WriteOptions& options = {});
```

Keep:

```cpp
[[nodiscard]] Status Write(const WriteBatch& batch,
                           const WriteOptions& options = {});
```

Existing calls continue to select `Write(const WriteBatch&)`; adding the fast
path does not change overload resolution or make references to
`Database::Write` ambiguous.

`WriteExclusive` is an explicit exclusive borrow. During the call, no thread
may read, copy, mutate, or submit the batch to either the same or another
database. The method may temporarily change the private sequence header but
restores it before returning or propagating an exception. It does not consume,
clear, or otherwise change the public batch contents.

`Write(const WriteBatch&)` copies once and delegates to the exclusive path. Its
cost is reported separately where batch ownership is relevant; it is not hidden
inside the primary `writebatch` parity case.

`Database::Put` and `Delete` construct one-entry mutable batches and use the
same fast path.

### WAL-creation durability control

Add to `Options`:

```cpp
bool sync_wal_creation = true;
```

When true, Modern preserves its current guarantee: before a newly created WAL
accepts writes, required file data and the containing directory entry are
synced. This applies to initial open and mutable-memtable rotation.

When false, Modern follows pinned LevelDB's WAL-creation behavior and performs
neither the initial empty-WAL sync nor the new-WAL directory sync. Record-level
`WriteOptions::sync`, SSTable file and directory sync, MANIFEST sync, CURRENT
installation, database-directory creation, and metadata-file creation are
unchanged.

The option is a durability control, not a generic performance switch. The
benchmark manifest records both the boolean value and a versioned semantic
marker. Production defaults remain unchanged.

## Implementation milestones

Implementation begins only after this ADR's documentation PR is reviewed and
merged. Each milestone is a sequential PR. A milestone must pass its correctness
gates and bounded review before the next starts. No write performance numbers,
profiles, or acceptance decisions are collected between milestones.

### Milestone 1: owned batches, queue, and direct leader groups

Files:

- `include/modern_leveldb/db.h`
- `include/modern_leveldb/write_batch.h`
- `src/api/database.cc`
- `src/api/write_batch.cc`
- `src/api/api_internal.h`
- `src/format/write_batch.{h,cc}`
- `src/engine/write_path.{h,cc}`
- directly related unit tests

Work:

1. Replace `EncodedWriteBatch`'s vector storage with string/inline-header
   storage while preserving byte-oriented views.
2. Encode Put/Delete directly after one checked reservation. Preserve strong
   error safety and add the alias-only staging path.
3. Add a trusted owned-batch reader distinct from checked external
   `WriteBatchReader::Open`.
4. Add `WriteExclusive` and preserve the existing const `Write` contract.
5. Move queue callbacks from stored `std::function` objects to templated direct
   dispatch.
6. Keep the leader batch direct when no follower joins. Populate reusable group
   scratch only after a compatible follower is found.
7. Avoid result allocation for a single writer. For a real multi-writer group,
   allocate shared result ownership before commit so post-commit completion
   remains `noexcept`.
8. Add an RAII sequence-restoration guard for the exclusive path.
9. Preserve exact sync compatibility, group limits, forced-writer isolation,
   wakeup order, and exception abort semantics.

### Milestone 2: WAL, trusted memtable insertion, skip list, and arena

Files:

- `src/format/wal_format.{h,cc}`
- `src/wal/wal_io.{h,cc}`
- `src/memory/memtable.{h,cc}`
- `src/memory/skiplist.h`
- `src/memory/arena.{h,cc}`
- `src/format/internal_key.{h,cc}`
- directly related unit and fault-injection tests

Work:

1. Replace the fragment vector with a cursor consumed by `WalWriter`.
2. Precompute initial CRCs for the four physical record types.
3. Preserve empty-record, exact-block-boundary, short-write, first-error,
   logical flush, and optional sync behavior byte for byte.
4. Add trusted memtable insertion and trusted skip-list insertion.
5. Use trusted internal-key comparison only for internally constructed memtable
   and lookup keys.
6. Allocate a node and all of its trailing atomic links in one arena allocation
   without a non-standard flexible array member.
7. Match pinned alignment and per-block owner-pointer accounting.
8. Retain checked recovery insertion and checked public/internal test surfaces.

### Milestone 3: make-room, rotation, sequence publication, and durability

Files:

- `include/modern_leveldb/options.h`
- `src/engine/database.{h,cc}`
- `src/engine/recovery.{h,cc}`
- `src/platform/file_system.h`
- platform and injected filesystem implementations as required
- database, recovery, and API tests

Work:

1. Use `VersionSet::current_raw()` under the DB mutex in make-room checks.
2. Preserve slowdown, immutable wait, L0 stop, and rotation ordering.
3. Add `Options::sync_wal_creation`, default true.
4. Route initial WAL creation and rotation through the selected durability
   policy with exact sync-call tests.
5. Keep all replacement objects constructed before state mutation and retain
   no-reuse file-number behavior.
6. Publish `last_sequence` only after successful memtable insertion and verify
   the sticky-failure coupling.
7. Preserve old-WAL close error propagation and scheduler failure semantics.

### Milestone 4: table construction, MANIFEST unlocking, and shutdown

Files:

- `src/engine/build_table.{h,cc}`
- `src/engine/flush.{h,cc}`
- `src/engine/compaction.{h,cc}`
- `src/table/table_builder.{h,cc}`
- `src/table/block_builder.{h,cc}`
- `src/metadata/version.{h,cc}`
- `src/metadata/version_set.{h,cc}`
- `src/engine/database.{h,cc}`
- related table, version, database, compaction, and failure tests

Work:

1. Add a trusted TableBuilder key/order path for validated iterators.
2. Retain the final recoverable 32-bit block-output boundary.
3. Add a trusted runtime VersionBuilder path using per-level additions,
   deletions, and ordered linear merge; retain the checked builder for
   MANIFEST recovery and malformed-edit tests.
4. Preserve exactly one database-directory sync after table creation and before
   an edit referencing that table can become durable.
5. Add a runtime `LogAndApply` form that receives the owning
   `std::unique_lock<std::mutex>&`.
6. Stage the candidate version and all potentially allocating install state
   before MANIFEST I/O.
7. Release the DB mutex around append, sync, and required CURRENT installation;
   reacquire it for every status and exception path before state installation
   or sticky-error recording.
8. Do not overwrite file numbers or sequences advanced by foreground writers
   while the lock was released.
9. Add a deterministic blocking-filesystem test proving a foreground writer can
   proceed while MANIFEST sync is blocked.
10. Add the missing closing check after a flush table is built and before its
   edit is applied.
11. Preserve pending-output protection, early immutable wakeup, cleanup rules,
   and exception containment.

### Milestone 5: fixed-work harness and retained evidence

Files:

- `benchmarks/profiling_bench.cc`
- `tools/run_performance.py`
- new `tools/run_write_parity.py`
- `tests/tools/performance_test.py`
- `tests/tools/performance_smoke.py`
- new `tests/tools/write_parity_test.py`
- benchmark contract tests
- `docs/write-profiling-design.md`
- this ADR's outcome section after measurement

Work:

1. Make Modern's `writebatch` adapter select either const `Write` or
   `WriteExclusive` without changing the operation stream.
2. Add explicit CLI selectors for Modern batch ownership and WAL-creation
   durability. Default batch ownership remains the existing const-copying path;
   the parity role selects exclusive ownership. Reject invalid
   role/option/workload combinations.
3. Record versioned provenance for selected batch ownership, WAL-creation
   policy, hardware CRC role, source/build identity, and binary hashes.
4. Version the mutable completion schema and extend reports, after the final
   verified close, with descriptive residual file counts and bytes for WAL,
   table, MANIFEST, and total regular database files.
5. Extend `tools/run_performance.py` to pass and validate the batch-ownership
   and WAL-creation roles, accept the new completion schema, and retain raw
   wall/process-CPU nanoseconds per benchmark iteration in addition to existing
   per-item values.
6. Retain `background_completion=not_drained` and
   `steady_state_claimed=false`.
7. Add `tools/run_write_parity.py` as the predeclared matrix driver and
   aggregator for every role/workload/round cell, pair order, fixed iteration
   count, completion fingerprint, expected provenance, and admission rule.
8. Reject calibration, framework repetition, duplicate cells, missing cells,
   ad hoc reruns, mixed frozen binaries, and aggregation that substitutes
   per-item timing for the primary per-iteration metric.

The executable and `tools/run_performance.py` use these exact selectors:

```text
--modern-write-batch-ownership copying|exclusive
--modern-wal-creation durable|leveldb
```

Defaults are `copying` and `durable`. Batch ownership other than `copying` is
valid only for `modern/writebatch/65536`; the WAL selector is valid only for
Modern mutable cases. LevelDB records both roles as `not_applicable`.

The benchmark context and retained manifest record:

```text
modern_write_batch_ownership
modern_write_batch_ownership_semantics=const-copy-v1|exclusive-borrow-v1|not_applicable
modern_wal_creation
modern_wal_creation_semantics=file-and-directory-before-write-v1|pinned-leveldb-v1|not_applicable
```

## Correctness and hardening gates

Every milestone runs the smallest directly covering tests first. Before the
completed implementation is eligible for measurement, run:

1. all unit and CMake consumer tests;
2. full Modern/pinned compatibility in both directions;
3. WAL, MANIFEST, table, recovery, file-lock, and crash/power-loss harnesses;
4. fuzz targets for batches, WAL, tables, versions, and database operation
   sequences;
5. ASan/UBSan and TSan presets;
6. Debug and Release builds on supported compiler families;
7. GCC 13 changed-code coverage using the repository's existing coverage
   policy;
8. benchmark contract and smoke tests, without collecting admission numbers;
9. one bounded GPT-5.6 Sol implementation review with all justified findings
   resolved.

Required regressions include:

- aliased Put/Delete source views;
- self-append and count overflow;
- exclusive-batch sequence restoration after success, returned error, thrown
  prepare, thrown commit, and multi-writer grouping;
- unchanged const `Write` behavior, function references, and copying;
- single-writer no-allocation and multi-writer preallocation contracts;
- every WAL fragment boundary and checksum;
- checked recovery rejection versus trusted owned insertion;
- fixed skip-list allocation count, alignment, and arena memory usage;
- exact WAL creation sync calls in both option modes;
- checked recovery VersionBuilder and trusted runtime delta-builder equivalence;
- large installed topologies avoiding per-file runtime map-node allocation and
  release overlap validation;
- exactly one table-directory barrier before a referencing MANIFEST edit is
  durable;
- writes advancing while MANIFEST sync is blocked;
- MANIFEST failure and exception lock reacquisition;
- close during unlocked flush construction;
- pending-output retention and no cleanup after sticky error.

## Final fixed-work measurement

### Frozen builds

Create separate unchanged source worktrees and build directories for:

1. the pre-parity Modern production baseline;
2. the final Modern candidate plus hardware-CRC LevelDB reference;
3. the canonical disabled-CRC LevelDB continuity control.

Do not rebuild a frozen binary between rounds. Retain:

- source and requested dependency revisions;
- dirty state and local patch;
- compiler and complete flags;
- compile commands;
- executable SHA-256;
- hardware-CRC, batch-ownership, and WAL-durability markers;
- the predeclared matrix manifest.

### Workloads

Use the existing fixed mutable workloads:

| Case | Corpus | Timed work per fresh process | Primary unit |
|---|---:|---:|---|
| `overwrite/65536` | 65,536 records | 262,144 acknowledged single-key overwrites | write call |
| `writebatch/65536` | 65,536 records | 8,192 acknowledged batches of 32 writes | batch call |
| `writesync/4096` | 4,096 records | 1,024 acknowledged sync writes | sync write call |
| `mixed50/65536` | 65,536 records | 262,144 read-then-write iterations | read/write iteration |

Each process:

1. creates a fresh database path;
2. loads the deterministic corpus;
3. closes, reopens, and verifies it;
4. performs the existing deterministic untimed warmup writes;
5. invokes the timed callback exactly once with its fixed operation count;
6. verifies, closes, reopens, verifies again, and closes;
7. writes the completion fingerprint and residual-file description.

Do not force background quiescence before, during, or after timing. The result
is acknowledged-operation latency and process CPU for this fixed lifecycle, not
a steady-state or fully drained compaction claim.

### Primary matched matrix

Compare final Modern with `sync_wal_creation=false` against hardware-CRC pinned
LevelDB. The Modern `writebatch` role uses `WriteExclusive`; other workloads
use their natural public operations.

Use five paired rounds and one callback per fresh process. Odd rounds run the
reference first; even rounds run Modern first. The matrix contains 40 timed
processes:

```text
4 workloads * 5 rounds * 2 engines
```

Wall time per timed iteration is primary. Process CPU per iteration is
supporting evidence. Also report wall and CPU normalized per written key for
`writebatch`; do not substitute that normalization for the primary batch-call
metric.

A round delta is:

```text
Modern / hardware-LevelDB - 1
```

The aggregate is the median of the five fresh-process values for each role,
and the aggregate delta is the ratio of those medians.

### Production regression matrix

Compare final Modern with `sync_wal_creation=true` against the frozen pre-parity
Modern binary, also using five alternating paired rounds and fresh paths. This
adds 40 timed processes. Both final and baseline `writebatch` roles use the
existing const-copying `Write` API so this matrix protects current users rather
than silently substituting the new exclusive API.

This matrix protects the production default from a result that passes only
after disabling stronger durability.

### Batch ownership control

Using the final candidate binary with `sync_wal_creation=false`, compare
`WriteExclusive` with const-copying `Write` for `writebatch/65536` in three
alternating paired rounds. This descriptive control adds six timed processes
and reports:

```text
copying / exclusive - 1
```

It has no admission threshold. It makes the preserved API convenience cost
visible without charging it to the primary parity mechanism.

### CRC continuity control

Compare hardware-CRC pinned LevelDB with canonical disabled-CRC pinned LevelDB
for the same four workloads using three alternating paired rounds. This is
descriptive and adds 24 timed processes. It has no admission threshold.

For `overwrite`, `writesync`, and `mixed50`, report the unpaired ratio of final
Modern's production and matched medians as the observed stronger-WAL-durability
cost. Do not compute that ratio for `writebatch`, whose two matrices
deliberately use different ownership modes, and do not describe any unpaired
ratio as a causal estimate when host noise or background layout differs.

The complete final design contains 110 timed fresh processes:

```text
40 primary + 40 production regression + 6 batch ownership + 24 CRC continuity
```

### Admission

Positive deltas mean the candidate is slower.

Write-path parity is complete only when all of the following hold:

1. For `overwrite`, `writebatch`, and `mixed50`, every matched aggregate wall
   delta is at most +5%, and no individual round is more than +10%.
2. For `writesync`, the matched aggregate wall delta is at most +5%, and at
   least four of five rounds are no more than +20%. Sync process CPU remains
   supporting evidence because wall time is filesystem dominated.
3. Every non-sync matched aggregate process-CPU delta is at most +5%.
4. In the production regression matrix, the final aggregate wall delta is at
   most +5% for non-sync workloads and +10% for `writesync`.
5. Every completion fingerprint matches across roles, every expected fixed
   operation count is exact, and all residual-file metrics are reported.
6. All correctness, compatibility, crash, sanitizer, compiler, coverage,
   benchmark-contract, and review gates pass.

The wider individual `writesync` rule is predeclared because eight existing
dirty local calibration pairs across several revisions showed sync wall deltas
from -26.95% to +32.43%, while their median was close to parity. Those runs are
not outcome evidence and cannot be spliced into the final matrix. They justify
five fresh-process rounds and one allowed sync outlier rather than an ad hoc
rerun policy.

No cell may be discarded or replaced. A documented external interruption
invalidates and restarts the entire affected matrix, not only an unfavorable
cell.

If the completed implementation misses admission, retain the failed matrix,
capture matched fixed-count CPU profiles for the failing cases, and make a new
reviewed design decision. Do not tune an intermediate milestone or weaken a
threshold after seeing results.

## Consequences

### Positive

- The normal write path has a precise pinned baseline instead of a collection
  of unrelated micro-optimizations.
- Stronger durability and error handling remain production defaults.
- Trusted internal operations have explicit, reviewable boundaries and checked
  external fallbacks.
- MANIFEST latency no longer unnecessarily serializes foreground work.
- Final performance claims distinguish checksum configuration, durability,
  foreground acknowledgement, process CPU, and residual background debt.

### Negative

- `WriteExclusive` requires an exclusive-borrow contract for a non-const batch.
- Trusted paths increase the importance of boundary tests and assertions.
- Exception-safe MANIFEST unlock/relock and non-throwing installation require
  careful staging.
- Exact skip-list layout and arena accounting couple internal memory behavior
  more closely to pinned LevelDB.
- The final measurement matrix is intentionally larger than prior three-round
  read matrices because sync-write variance is materially higher.

## Rejected alternatives

### Keep all release validation because it is safer

Rejected. Repeating validation after ownership and sequence invariants are
established does not improve handling of external corruption. It only moves
boundary work into every successful write.

### Optimize isolated helpers and measure each one

Rejected. Neighboring costs and background work interact, and prior read-path
experiments demonstrated that plausible local improvements can fail end-to-end
admission.

### Weaken Modern's production durability to match LevelDB

Rejected. The current default is a deliberate stronger guarantee. An explicit
matched control makes the comparison honest without silently changing users'
durability.

### Use canonical disabled-CRC LevelDB as the only write reference

Rejected. WAL and table checksums are on the measured path. Comparing different
CRC implementations would attribute configuration cost to engine design.

### Force both engines to drain background work

Rejected. That changes the acknowledged-write contract, requires
engine-specific control surfaces, and does not represent either public API.
Residual files are reported and the non-steady-state limitation remains
explicit.

### Publish performance after each milestone

Rejected. Intermediate results invite local tuning before the complete pinned
mechanism exists and violate the parity-first development sequence.
