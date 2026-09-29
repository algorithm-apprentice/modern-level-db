# ADR-0057: Pinned LevelDB Version and Output Parity

## Status

Implemented. The design review completed the read-pin acquisition and
current-version identity APIs, restored pinned unchanged-on-miss output,
defined result-ownership provenance for every workload, and added every
superseded ADR surface. The bounded GPT-5.6 Sol implementation review found
three test gaps rather than production defects: distinguishing lazy selection
from unopened eager selection, pinning an already-installed immutable
memtable, and consuming a second-file error charge before returning it. The
implementation adds direct regression coverage for all three.

## Context

ADR-0053 requires the complete pinned Google LevelDB point-read path before
one final integrated performance decision.

The merged milestones now provide:

- PR #66: pinned intrusive table/block cache ownership.
- PR #68: lazy checked block decoding, reusable `std::string` iterator keys,
  trusted table comparison, and end-to-end corruption propagation.
- PR #70: mapped-uncompressed borrowing, block-cache bypass, default
  count-limited mmap, copied-read control, and diagnostic schema 4.

The remaining measured-path differences are above the table:

1. Modern materializes one vector containing every level-0 and deeper-level
   table candidate before searching any table.
2. Seek charging is reconstructed from that completed vector after the read
   decides, rather than recorded when the second file is about to be searched.
3. `DatabaseEngine::Get` copies three `shared_ptr`s under the mutex to keep the
   mutable memtable, immutable memtable, and version alive.
4. Public `Database::Get` copies the shared database state for a synchronous
   call whose `Database` object must already remain alive.
5. Every successful value read creates a new owning vector and passes that
   allocation through Table, lookup, engine, and public API layers.

Pinned LevelDB instead:

- Builds only the overlapping level-0 temporary list.
- Searches deeper levels one at a time and stops immediately.
- Records the first file for seek charging when it reaches a second file,
  including when the second table read fails.
- Pins memtables and the current version under the DB mutex, releases them
  under that mutex after the read, and performs no shared control-block
  increments.
- Writes directly into the caller's reusable `std::string`.

The project is alpha and has no production deployments. Public and internal
interfaces may break to reach the smallest coherent final design. Do not add
migration or compatibility machinery for current APIs or diagnostic reports.

## Scope

This ADR covers ADR-0053 Milestone 4:

- Lazy level/file visitation for point reads and read samples.
- Seek charging at the pinned decision point.
- Typed synchronous-Get read pins for memtables and versions.
- Removal of per-call public database-state shared ownership.
- Reusable value output from table lookup through the public API.
- A retained owning public convenience overload.
- Benchmark selection of reusable versus owning result behavior.
- Diagnostics needed to measure lazy selection without including table I/O.
- Directly related ADRs, public documentation, tests, and fuzz contracts.

It does not cover:

- Replacing long-lived iterator or compaction ownership with read pins.
- Replacing `Version::File` shared ownership.
- Changing snapshot or iterator child-handle lifetime guarantees.
- `MultiGet`, merge operands, range deletions, row caches, or file indexing.
- Iterator value ownership.
- A Milestone-4-only throughput admission test.

## Pinned field and function mapping

| Pinned LevelDB mechanism | Modern mechanism | Decision |
|---|---|---|
| `Version::ForEachOverlapping` | lazy internal `ForEachOverlapping` helper | L0 temporary list, then deeper levels one at a time with immediate stop |
| `Version::Get::State::last_file_read` | previous visited `Candidate` | Record the first seek charge immediately before visiting the second candidate |
| `Version::GetStats` | optional `SeekCharge` output | Preserve a charge even when the deciding table call returns an error |
| `Version::RecordReadSample` | `SampleCharge` | Reuse the lazy visitor and stop after two matches |
| `MemTable::Ref/Unref` for Get | `MemTable::ReadPin` | Non-atomic read pin acquired/released under the DB mutex |
| `Version::Ref/Unref` for Get | `VersionSet::ReadPin` | Non-atomic read pin plus retained old-current owner |
| DBImpl local `mem/imm/current` | `DatabaseEngine::ReadSources` | One typed scope holds all synchronous read pins |
| caller `std::string* value` | caller `vector<byte>& value` | Reuse capacity and copy once from the deciding source |
| DBImpl raw object during call | raw `DatabaseState*` from live facade | No per-call `shared_ptr` increment for synchronous operations |
| existing convenience API | owning `optional<vector<byte>>` wrapper | One vector delegates to the reusable core |

## Lazy version visitation

### Replace all-level candidate materialization

Remove the current `Candidates` function and its vector containing every level.

Add one internal templated visitor in `engine/lookup.cc`:

```cpp
template <typename Visitor>
Status ForEachOverlapping(
    const Version& version,
    const InternalKeyComparator& comparator,
    ByteView user_key,
    ByteView internal_key,
    Visitor&& visitor);
```

The visitor receives:

```cpp
struct Candidate {
  std::uint32_t level;
  const Version::File* file;
};
```

The pointer identifies the stable shared-file slot inside the pinned
`Version`. It is valid only while that version's read pin or another outer
owner remains alive.

Traversal matches pinned LevelDB:

1. Scan level 0 and collect only files whose user-key range contains the key.
2. Sort that temporary list by descending file number.
3. Visit level-0 candidates in that order, stopping when the visitor returns
   false or an error.
4. For each deeper level only after all earlier visits continue:
   - Binary-search its first file whose largest internal key is not before the
     lookup internal key.
   - Reject it when its smallest user key is after the lookup user key.
   - Visit the one remaining candidate.
5. Stop immediately after the deciding value, deletion, corruption, or table
   error.

The only candidate allocation retained is pinned LevelDB's overlapping
level-0 temporary list. Do not cache a cross-level cursor or add RocksDB's
`FileIndexer`.

`level0_candidates` continues to count the full overlapping L0 temporary
list. `deeper_candidates` counts candidates as they are produced; levels
after a stop perform no search and add no count.

### Preserve comparator and range behavior

Use the internal comparator for deeper-level largest-key binary search and
the user comparator for smallest/largest user-key range checks. Reverse and
custom comparator behavior remains covered.

Do not parse or copy file metadata during traversal. The version already owns
typed `InternalKey` boundaries and shared file metadata.

## Seek charging at the decision point

Change `SeekCharge` to borrow the visited file slot:

```cpp
struct SeekCharge {
  std::uint32_t level;
  const Version::File* file;
};
```

The lookup's version read pin keeps the slot and metadata alive until the
engine consumes the charge under the mutex.

The file visitor tracks the previous candidate. Immediately before it visits a
second candidate for the first time, it records the previous candidate as the
seek charge. Therefore:

- A first candidate that decides the read is not charged.
- A miss in the first candidate followed by any second candidate charges the
  first.
- An error opening or reading the second candidate still charges the first.
- A read that searches exactly one candidate and misses does not charge.
- Later candidates do not replace the first charge.

The lookup API uses an explicit output parameter so an error and its charge
can both survive:

```cpp
Result<bool> LookupValue(
    const MemTable& memtable,
    const MemTable* immutable,
    const Version& version,
    TableCache& table_cache,
    const InternalKeyComparator& comparator,
    const LookupKey& key,
    std::vector<std::byte>& value,
    std::optional<SeekCharge>& seek,
    const TableReadOptions& options = {});
```

`seek` is reset at entry. It may be populated whether the result succeeds or
fails. The boolean is true only for a value; missing and deletion are false.

`DatabaseEngine::Get` reacquires the mutex, applies a populated charge, and
only then returns the lookup result or error.

`SeekStatistics::Charge` receives the pinned read version by reference and the
current version through the existing synchronized VersionSet. It copies a
`Version::File` shared owner only when a budget entry or seek compaction must
retain that metadata; traversal and ordinary charges keep only borrowed
pointers.

`SampleCharge` uses the same visitor, remembers the first match, and stops
after the second. It allocates only the L0 temporary list.

## Typed synchronous read pins

### Keep long-lived ownership unchanged

Database iterators and compactions may outlive one engine call and already use
shared ownership to integrate with public child-handle lifetimes,
`VersionSet::LiveFiles`, and background work. This milestone does not rewrite
those paths.

Only synchronous `Get` replaces its three per-call `shared_ptr` copies.

### Add `MemTable::ReadPin`

`MemTable` gains a move-only nested read pin and a non-atomic read-pin count:

```cpp
class MemTable::ReadPin final {
 public:
  ReadPin(const ReadPin&) = delete;
  ReadPin& operator=(const ReadPin&) = delete;
  ReadPin(ReadPin&&) noexcept;
  ReadPin& operator=(ReadPin&&) = delete;
  ~ReadPin();

  [[nodiscard]] const MemTable& value() const noexcept;

 private:
  friend class MemTable;
  explicit ReadPin(const MemTable& table) noexcept;
  const MemTable* table_ = nullptr;
};

[[nodiscard]] ReadPin MemTable::PinRead() const noexcept;
```

Creating a pin increments `read_pins_`; destroying it decrements. Both require
the database mutex. The count is deliberately non-atomic, matching LevelDB's
mutex-protected refs.

`DatabaseEngine` retains the existing shared owners for the installed mutable
and immutable memtables. When a flushed immutable memtable is removed while
its read-pin count is nonzero, move that shared owner into
`read_pinned_memtables_`. Once pins release, erase retained owners whose count
is zero.

Moving the mutable field into the immutable field transfers its installed
owner without changing the read-pin count.

### Add `VersionSet::ReadPin`

`Version` gains a mutex-protected read-pin count. `VersionSet` adds:

```cpp
class VersionSet::ReadPin final {
 public:
  ReadPin(const ReadPin&) = delete;
  ReadPin& operator=(const ReadPin&) = delete;
  ReadPin(ReadPin&&) noexcept;
  ReadPin& operator=(ReadPin&&) = delete;
  ~ReadPin();

  [[nodiscard]] const Version& value() const noexcept;
};

[[nodiscard]] ReadPin PinCurrent() noexcept;
```

The pin stores a raw `const Version*` and its owning `VersionSet*`;
construction and destruction require the external database mutex.

When `Install` replaces a current version whose read-pin count is nonzero, move
the old current shared owner into `read_pinned_versions_`. Releasing the last
read pin erases that retained owner when the version is no longer current.

The existing weak installed-version list remains responsible for
`LiveFiles()`, including versions held by iterators or compactions. A retained
read-pinned version is visible through the same weak entry, so obsolete-file
cleanup cannot remove its files.

Add a borrowed synchronized accessor:

```cpp
[[nodiscard]] const Version* current_raw() const noexcept;
```

`current_raw()` is the ordinary identity/value accessor and performs no
control-block operation.

`SeekStatistics::Charge` compares the read-pinned version with
`current_raw()`. ADR-0053's Milestone 5 audit correction supersedes the
original weak-owner design: budgets live in shared file metadata, the recorded
version and file slot remain borrowed, and only background
`FileToCompact` materializes the owning file.

### Capture all three sources in one scope

Add a private `DatabaseEngine::ReadSources` scope:

```cpp
class DatabaseEngine::ReadSources final {
 public:
  ReadSources(DatabaseEngine& engine,
              std::unique_lock<std::mutex>& lock) noexcept;
  ~ReadSources();

  [[nodiscard]] const MemTable& memtable() const noexcept;
  [[nodiscard]] const MemTable* immutable() const noexcept;
  [[nodiscard]] const Version& version() const noexcept;
};
```

Construction requires the lock and creates mutable, optional immutable, and
current-version pins. Destruction asserts that the same lock is owned, resets
the pins, and removes released retained memtable owners.

`DatabaseEngine::Get` follows:

1. Lock the mutex and select the sequence.
2. Construct `ReadSources`.
3. Unlock.
4. Build the lookup key and perform the read.
5. On every exception, relock before the pin scope unwinds, then rethrow.
6. Relock on ordinary completion.
7. Apply seek charging even when the lookup result is an error.
8. Destroy `ReadSources` while locked.
9. Return the result.

No shared control block is incremented or decremented on the ordinary Get
path.

Object destruction or movement concurrent with a method on the same public
handle remains outside the API contract.

## Public database-state ownership

The public `Database` retains its `shared_ptr<DatabaseState>` member because
snapshots and iterators intentionally keep the engine alive after the
`Database` handle is destroyed.

Synchronous methods use `state_.get()` rather than copying the shared pointer.
The live public object is the owner for the duration of the call.

For synchronous `Get`, a supplied `Snapshot` must remain alive for the call,
so validation reads its registration through a raw pointer without copying
the registration shared pointer.

`NewIterator` and `GetSnapshot` still copy shared ownership into returned
child handles. This is outside the per-Get path and preserves the public RAII
lifetime guarantee.

Apply the same raw-state rule to synchronous Put, Delete, and Write for a
single consistent facade contract.

## Reusable result ownership

### Add the public reusable overload

Add:

```cpp
Result<bool> Database::Get(
    ByteView key,
    std::vector<std::byte>& value,
    const ReadOptions& options = {});
```

- `true`: a visible value was found and written to `value`.
- `false`: the key is missing or its visible entry is a deletion; `value`
  remains unchanged, matching pinned LevelDB.
- Error: `value` remains valid but its content is unspecified.
- An empty stored value returns true with an empty vector.

Keep:

```cpp
Result<std::optional<std::vector<std::byte>>> Database::Get(
    ByteView key,
    const ReadOptions& options = {});
```

The owning overload creates one vector, calls the reusable overload, and
returns either `nullopt` or the moved vector. It contains no independent read
logic. The public read-diagnostic `GetScope` belongs only to the reusable core,
so the wrapper does not double-count one public call.

### Carry the buffer through every layer

Change the private layers to write the same vector:

```cpp
enum class TableLookupKind {
  Missing,
  Value,
  Deletion,
};

Result<TableLookupKind> Table::Get(
    const LookupKey& key,
    std::vector<std::byte>& value,
    const TableReadOptions& options = {}) const;

Result<bool> DatabaseEngine::Get(
    ByteView key,
    std::vector<std::byte>& value,
    const DatabaseEngineReadOptions& options = {});
```

`LookupValue` uses the signature in the seek-charging section.

For a found value, resize the destination and copy bytes into its existing
storage. Do not construct an intermediate vector. Missing/deletion paths do
not mutate it.

The result-copy diagnostic stage surrounds only the one final copy from the
deciding memtable or table entry. `result_bytes` remains the returned value
size.

Remove the old owning `TableLookup` and `PointRead` value containers.

## Candidate-selection diagnostics

Lazy deeper-level selection is interleaved with table reads, so one ordinary
`StageScope` cannot cover the whole selection phase without incorrectly
including table I/O.

Add a diagnostic-only segmented accumulator:

```cpp
class StageAccumulator final {
 public:
  explicit StageAccumulator(Stage stage) noexcept;
  void Resume() noexcept;
  void Pause() noexcept;
  ~StageAccumulator();
};
```

For a sampled Get:

- It accumulates any number of selection-only intervals.
- Its destructor records one stage event with the sum.
- L0 scan/sort is one interval.
- Each deeper-level binary search/range check is one interval.
- Table-cache and table work occur while it is paused.

This preserves:

```text
candidate_selection.events == sampled_gets
```

for the fixed table-backed diagnostic corpus without misattributing I/O.
Normal builds compile out the accumulator with the existing diagnostics
preprocessor boundary.

## Benchmark and diagnostic contracts

### Result-ownership control

Modern read-family benchmarks default to the reusable overload. Add:

```text
--modern-result-ownership reusable|owning
```

- `reusable`: one adapter-owned vector is reused across Gets.
- `owning`: call the convenience overload and destroy its result each Get.

The non-default owning control is valid only for Modern `readrandom` and
`readmissing`. Scan and seek-reuse do not use point-Get result ownership.
Mutable workloads retain their fixed contract and use reusable reads where
applicable.

Reports and validators require:

```text
modern_result_ownership_semantics=reusable-get-v1
modern_result_ownership=reusable|owning|not_applicable
```

The semantics field describes the compiled Modern adapter capability:

| Engine/workload | `modern_result_ownership_semantics` | `modern_result_ownership` |
|---|---|---|
| Modern `readrandom`/`readmissing` | `reusable-get-v1` | selected `reusable` or `owning` |
| Modern `mixed50` | `reusable-get-v1` | fixed `reusable` |
| Modern scan/seek-reuse/write-only mutation | `reusable-get-v1` | `not_applicable` |
| LevelDB any workload | `not_applicable` | `not_applicable` |

The runner rejects a non-default ownership control outside Modern
`readrandom`/`readmissing`. This matrix rejects pre-Milestone-4 binaries whose
Modern adapter always used the owning overload without mislabeling workloads
that perform no point Get.

The final ADR-0053 matrix uses `reusable`; separate owning runs report the
convenience-overload tax.

### Read-diagnostic schema 5

Bump new read-diagnostic reports from schema 4 to schema 5.

Schema 5 means:

- Lazy table-file visitation and immediate stop.
- Seek charging at the second-file decision point.
- Typed synchronous read pins.
- Reusable output through the whole Get path.
- Segmented candidate-selection timing.
- The schema-4 mmap/count-only contract remains in force.

New tooling supports schema 5 only. Do not add schema-4 migration or
cross-schema comparison code.

The fixed diagnostic runner always uses reusable output and records the result
ownership semantics in build provenance. Existing counter invariants remain,
but deeper-candidate totals may fall because levels after a deciding source
are no longer precomputed.

## Documentation updates

The implementation PR updates:

- ADR-0030: lazy visitation, charge timing, borrowed seek charge, and reusable
  output.
- ADR-0031: synchronous Get pins replace shared copies; long-lived iterator
  ownership remains shared.
- ADR-0038: reusable public Get, synchronous raw state access, and child-handle
  ownership boundaries.
- ADR-0025: replace owning `TableLookup` with caller-buffer lookup kind.
- ADR-0027: add read-pinned current-version retention and borrowed current
  accessors.
- ADR-0036: charge before the second file and retain charges on later errors.
- ADR-0037: replace owning engine Get with caller-buffer output and typed
  source capture.
- ADR-0042: Modern parity benchmarks use reusable output and expose an owning
  control.
- ADR-0053: Milestone 4 completion status.
- README and public headers: reusable Get semantics and owning convenience.
- Profiling documentation/help: result-ownership control, provenance, and
  schema 5.

## Validation plan

### Lazy visitation and charging tests

- L0 overlap collection/order matches pinned newest-first behavior.
- Deeper levels are selected only when every earlier visit continues.
- A deciding L0 or shallow-level value prevents all later binary searches and
  table opens.
- Reverse/custom comparators preserve boundaries.
- The first file is charged immediately before the second visit.
- A second-file open/read/corruption error still returns the first charge.
- One-file misses and first-file errors do not charge.
- Read sampling stops after two overlaps and performs no later-level work.
- Diagnostic L0/deeper counters and segmented stage event counts match the
  new contract.

### Read-pin lifetime tests

- A Get paused in table I/O remains valid while another thread switches and
  flushes its captured mutable/immutable memtable.
- The same paused Get remains valid while a new current version is installed.
- `LiveFiles()` retains files of a read-pinned old version until the Get
  releases it.
- Pins release on success, missing, deletion, table error, lookup-key error,
  and exception paths.
- No retired memtable/version owner remains after the final pin releases.
- TSan covers concurrent reads, writes, flushes, and version installation.

### Reusable output tests

- Found values reuse existing capacity without an intermediate vector.
- Empty values return true.
- Missing and deletion return false without changing size, contents, or
  capacity.
- Snapshot reads, custom comparators, filters, mapped/copy modes, and every
  compression mode return identical bytes.
- Errors leave the vector valid and do not become false/missing results.
- The owning wrapper preserves the existing optional result behavior.
- Code review and optimized inspection prove that synchronous Get reads raw
  database-state/snapshot-registration pointers and does not increment their
  shared ownership.

### Tooling and repository gates

- Benchmark parsers reject missing/legacy result-ownership provenance.
- Reusable and owning point-read controls exercise their exact public
  overloads.
- Schema-5 diagnostic reports preserve stage/counter/build invariants.
- Full native unit/public API tests.
- ASan/UBSan and TSan.
- LevelDB compatibility, model, and crash suites.
- Format and database fuzz smoke.
- AppleClang Release and GCC warning-clean builds.
- 100% changed-code line/branch coverage.
- Bounded GPT-5.6 Sol implementation review.

This milestone receives no standalone throughput admission test.

## Sequential implementation order

After this design PR is reviewed and merged:

1. Implement lazy overlap visitation and exact seek-charge timing.
2. Add MemTable/VersionSet read pins and switch synchronous Get capture.
3. Add reusable Table/lookup/engine/public output and the owning wrapper.
4. Update benchmark controls, diagnostics schema 5, and documentation.
5. Run the complete Milestone 4 validation and implementation review.
6. Deliver one implementation PR and merge it before integrated Milestone 5.

Do not implement these steps on parallel branches.

## Rejected alternatives

### Keep the all-level candidate vector

Rejected. It performs deeper-level searches and allocation before an earlier
source can stop the read.

### Charge after the lookup returns

Rejected. It loses pinned behavior on second-file errors and requires
reconstructing traversal history.

### Replace all version and memtable ownership

Rejected. Long-lived iterators, compactions, and public child handles already
have correct shared lifetime contracts. Milestone 4 removes only the measured
per-Get copies and retains old objects while typed read pins need them.

### Use atomic intrusive read counts

Rejected. Get acquires and releases pins under the database mutex as pinned
LevelDB does. Atomics would preserve the hot-path ownership cost this
milestone removes.

### Hold the database mutex through the read

Rejected. Table I/O would block writers, flushes, and compactions and would
not match pinned control flow.

### Return a view into memtable or block storage

Rejected. Public values must outlive the internal read pins and table handles.
The caller-owned vector is the lifetime boundary.

### Remove the owning convenience overload

Rejected. The final API needs both allocation-free repeated reads and a simple
one-shot call. The wrapper has one implementation path and its tax is measured
separately.

### Add `MultiGet` or a row cache

Rejected. Neither is required for pinned point-read parity.

### Keep diagnostic schema 4

Rejected. Candidate-selection timing and result-ownership semantics change;
using the same schema number would accept incompatible reports.

### Add a Milestone-4 performance vote

Rejected by ADR-0053. Performance is measured only after integrated parity.

## Consequences

- Point reads allocate only the pinned L0 temporary list and stop before
  selecting unneeded deeper candidates.
- Seek charging matches pinned success, miss, and error timing.
- Ordinary synchronous Get performs no memtable/version/public-state
  shared-control-block increments.
- Long-lived iterator, compaction, snapshot, and engine lifetimes remain safe.
- Applications may reuse value capacity while the owning one-shot API remains
  available.
- The benchmark can report the convenience allocation tax without hiding it
  inside the parity result.
- Remaining work is the integrated ADR-0053 checklist audit, final hardening
  review, and one end-to-end performance matrix.

## References

- [ADR-0025 SSTable reader](0025-sstable-reader.md)
- [ADR-0027 version set](0027-version-set.md)
- [ADR-0030 point reads](0030-point-reads.md)
- [ADR-0031 iterators](0031-iterators.md)
- [ADR-0036 seek statistics](0036-seek-statistics.md)
- [ADR-0037 database engine](0037-database-engine.md)
- [ADR-0038 public RAII API](0038-public-raii-api.md)
- [ADR-0042 benchmark foundation](0042-benchmark-profiling-foundation.md)
- [ADR-0053 complete read-path parity](0053-leveldb-read-path-parity.md)
- [ADR-0056 table/mmap parity](0056-leveldb-table-mmap-parity.md)
- [Pinned LevelDB `DBImpl::Get`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [Pinned LevelDB `Version::Get`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [Pinned LevelDB public Get API](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/db.h)
