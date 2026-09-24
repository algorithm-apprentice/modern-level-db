# ADR-0033: Memtable Flush

- Status: Accepted
- Date: 2026-09-24

## Context

When the memtable fills, the engine makes it immutable and starts a new
memtable and write-ahead log ([ADR-0032](0032-write-path.md)). A flush then
writes the immutable memtable to a table
([ADR-0028](0028-level0-table-building.md)) and records the table in a
version edit that also marks the logs before the new one as obsolete, so that
the version set ([ADR-0027](0027-version-set.md)) can install it.

This implements only the `implement-flush` DAG node: writing a memtable's
table, choosing its level, and producing the edit that installs it. Making
room for a write, which switches memtables and logs and delays writes when
level 0 has too many files, scheduling background work, protecting the new
table's number from obsolete-file cleanup, applying the edit, dropping the
immutable memtable, and recording the error that stops writes belong to the
engine node, because they act on the engine's state under its mutex.
ADR-0032 left making room to the flush and engine nodes; this ADR assigns it
wholly to the engine node.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Engine background work | Write the immutable memtable to a table without the database mutex, choose its level from the version the flush started from, and get an edit that installs it and advances the log number |

No current caller requires flushing several immutable memtables into one
table, atomic flushes across column families, flushing to anything but a
table file, or asking other code whether a level overlaps a key range.

## Prior art

### Google LevelDB

Adopt `DBImpl::CompactMemTable`, `DBImpl::WriteLevel0Table`,
`Version::PickLevelForMemTableOutput`, and `Version::OverlapInLevel`:

- The engine allocates the table's number and keeps it from obsolete-file
  cleanup, builds the table with the mutex released, and, if the memtable
  produced a table, adds it to an edit at the level that the version current
  when the flush started picks.
- The level is 0 if a level-0 file overlaps the table's user-key range.
  Otherwise the table moves down while it is above level 2
  (`kMaxMemCompactLevel`), the next level has no file overlapping the range,
  and the files of the level after that overlapping the range total at most
  ten times the target file size (`MaxGrandParentOverlapBytes`), so that the
  table can later be compacted without rewriting too much data.
- Overlap compares user keys, both ends included. Level 0 checks every file;
  deeper levels hold sorted, disjoint files, so a binary search finds the
  only candidate.
- The edit sets the log number to the current log's and the previous log
  number to zero, even when the memtable was empty and produced no table, so
  that the logs before the current one become obsolete.
- The engine applies the edit, then drops the immutable memtable and removes
  obsolete files, or records a background error.

Change:

- **The directory is synced before the edit.** LevelDB never syncs the
  directory entry of a new table. Here the flush syncs the directory before
  it returns an edit that references the table, as
  [ADR-0011](0011-filesystem-contracts.md) and ADR-0028 require.
- **A typed edit.** The flush returns the edit, or an error, instead of
  filling the engine's edit and recording statistics.

### RocksDB

RocksDB's `FlushJob` always adds the table at level 0, can merge several
immutable memtables into one table, flushes column families atomically, and
can rewrite a memtable into a new memtable instead of a table (MemPurge).
Writing every flush to level 0 is rejected to keep LevelDB's level choice, so
that the file layout matches the format oracle's; the other features are
rejected because no current caller needs them.

## Decision

Add `src/engine/flush.{h,cc}`:

```cpp
struct FlushOptions {
  TableBuilderOptions table_options;
  std::uint64_t target_file_size = 2 * 1024 * 1024;
};

inline constexpr std::uint32_t MaxMemTableOutputLevel = 2;

std::uint32_t PickLevelForMemTableOutput(const Version& version,
                                         const Comparator& user_comparator,
                                         ByteView smallest_user_key,
                                         ByteView largest_user_key,
                                         std::uint64_t target_file_size);

Result<VersionEdit> FlushMemTable(FileSystem& file_system,
                                  const std::filesystem::path& directory,
                                  const InternalKeyComparator& comparator,
                                  const FlushOptions& options, TableCache& table_cache,
                                  const MemTable& memtable, std::uint64_t number,
                                  const Version& base, std::uint64_t log_number);
```

- `PickLevelForMemTableOutput` returns LevelDB's level for a table with that
  user-key range: 0 if a level-0 file overlaps it, and otherwise the first
  level `L` below `MaxMemTableOutputLevel` for which a file of level `L + 1`
  overlaps the range or the files of level `L + 2` that overlap it total more
  than ten times `target_file_size`, or `MaxMemTableOutputLevel` if there is
  none. A file overlaps the range if it holds a user key from
  `smallest_user_key` to `largest_user_key`, both included; level 0 checks
  every file, and deeper levels binary-search their sorted, disjoint files.
  It requires a range whose smallest key is not after its largest, the user
  comparator that orders the version's keys, and a target file size whose
  tenfold fits in 64 bits. The overlap queries stay private to the flush.
- LevelDB checks `level + 2 < kNumLevels` before it sums the grandparent
  files, which always holds for a level below `MaxMemTableOutputLevel`. Here
  a static assertion that `MaxMemTableOutputLevel + 1 < NumLevels` replaces
  the runtime check.
- `FlushOptions::target_file_size` is LevelDB's `max_file_size`, the size at
  which compaction will split its outputs. The flush accepts any size, so
  that tests can use small limits; the engine passes the size clipped to
  LevelDB's range of 1 MiB to 1 GiB where it sanitizes its options, as
  LevelDB's `SanitizeOptions` does, so that it picks LevelDB's levels.
- `FlushMemTable` builds the memtable into table `number` with
  `BuildTable`. If the memtable is empty, it performs no file operation and
  returns an edit without files. Otherwise it syncs the directory and returns
  an edit that adds the table at the level `PickLevelForMemTableOutput`
  picks in `base` for the table's user-key range. The edit sets the log
  number to `log_number` and the previous log number to zero.
- If building fails, `BuildTable` has tried to remove the file and ignored an
  error from the removal, so an unreferenced file may remain. If syncing the
  directory fails, the table stays in the directory and in the table cache.
  Either way the flush returns the error, and obsolete-file cleanup may
  remove the file once that is safe.
- `FlushMemTable` touches only its arguments, so the engine calls it without
  the database mutex while it holds `base` and the immutable memtable. The
  number must be a fresh file number that the caller keeps from obsolete-file
  cleanup, and `log_number` must be the number of the log that the memtable
  after the flushed one writes to.

The engine's flush allocates the number and records `base` with the mutex
held, calls `FlushMemTable` with it released, and then, with the mutex held
again, applies the edit with `LogAndApply` and either drops the immutable
memtable or records the error that stops writes. The engine must also:

- Sync the directory after it creates a log and before the log accepts a
  write, as recovery does, because the edit makes that log the oldest one
  that recovery replays, even when the memtable was empty and the flush
  synced nothing.
- Stop obsolete-file cleanup until the database reopens once `LogAndApply`
  fails, as LevelDB does after a background error, because the failed edit
  may still be durable and reference the table. A failure before
  `LogAndApply` only leaves a file that no edit references.

## Explicitly deferred behavior

- Making room for writes, background scheduling, pending outputs,
  obsolete-file cleanup, the shutdown check, option sanitizing, and flush
  statistics, which belong to the engine node.
- Shared overlapping-file queries, with open ends or level-0 range
  expansion, which belong to the compaction node.

## Validation plan

Unit tests cover:

- `PickLevelForMemTableOutput` for each reason to stop: a level-0 overlap
  with overlapping files in any order, an overlap in the next level for
  ranges before, between, touching, inside, spanning, and after its files,
  and grandparent files totaling more than ten times the target file size at
  both levels it checks, with a total exactly at the limit not stopping it;
  the level-2 cap; and a reverse comparator.
- `FlushMemTable` through an in-memory file system: the table's operations
  followed by the directory sync, the edit's file, level, and log numbers, an
  empty memtable that performs no file operation, and a failure at every file
  operation.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

An isolated differential helper has unmodified Google LevelDB create fresh
databases and, in one session each, write narrow key ranges that it flushes
at once, between range compactions that fill deeper levels. It then replays
each MANIFEST with Modern LevelDB's version builder and checks every flush
record: a record that adds exactly one file and deletes none, since every
compaction and trivial move deletes its inputs and a fresh database's
snapshot and recovery records add no file. LevelDB applies all edits after
opening on its single background thread, and a flush applies its edit before
any other, so the version before a flush record is the version that the
flush started from; `PickLevelForMemTableOutput` on that version must return
the level of the record's file. The helper runs until it has checked tables
at levels 0, 1, and 2 and every reason to stop.

## Consequences

- The engine's flush is one tested call between its locked steps.
- A memtable whose keys do not overlap lower levels skips compactions, as in
  LevelDB.

## References

- [Google LevelDB `DBImpl::CompactMemTable` and `WriteLevel0Table`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [Google LevelDB `Version::PickLevelForMemTableOutput`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [RocksDB `FlushJob`](https://github.com/facebook/rocksdb/blob/main/db/flush_job.cc)
