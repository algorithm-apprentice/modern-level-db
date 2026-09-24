# ADR-0028: Level-0 Table Building

- Status: Accepted
- Date: 2026-09-24

## Context

Recovery writes the memtables that it rebuilds from write-ahead logs to
level-0 tables, and a flush writes the immutable memtable to a level-0 table
before recording it in a version. Both need the same step: write a memtable's
entries to a new table file, make the file durable, and check that the table
opens.

The DAG placed that step inside the flush node, which comes after recovery
and also depends on the write path's memtable and WAL lifecycle. This ADR
adds the `implement-table-build` node before recovery, so that recovery and
flush both depend on it. Choosing the file number, the level, the MANIFEST
edit, and syncing the directory remain with those callers.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Recovery | Write each memtable that fills during WAL replay, and the last one unless recovery keeps it, to a new level-0 table and record the tables' metadata in one edit |
| Flush | Write the immutable memtable to a new table and record its metadata in an edit |

No current caller requires building tables from other iterators, output
validation beyond opening the table, table properties, or file checksums.

## Prior art

### Google LevelDB

Adopt LevelDB's `BuildTable`:

- An empty memtable produces no file.
- Otherwise the entries are added in order to a table builder writing
  `<number>.ldb`; the smallest and largest keys are the first and last
  entries' internal keys.
- The table is finished, synced, and closed, and then opened through the
  table cache to check that it is usable, which also caches it.
- Any failure removes the file.
- The caller allocates the file number, keeps it from obsolete-file cleanup
  while the table is written, and records the table in an edit.

Change:

- **The table builder finishes durably.** `TableBuilder::Finish` already syncs
  and closes the file ([ADR-0024](0024-sstable-writer.md)), so building does
  not sync or close it separately.
- **Typed result.** LevelDB reports an empty memtable by leaving a zero file
  size in its output metadata. Here building returns the table's
  `FileMetadata`, or nothing for an empty memtable.
- **Directory sync by the caller.** LevelDB never syncs the directory entry of
  a new table. Here the caller syncs the directory before a MANIFEST edit
  references the table, as [ADR-0011](0011-filesystem-contracts.md) requires,
  so recovery can sync once for all of its tables.

### RocksDB

RocksDB's `BuildTable` adds compaction filters, range deletions, blob files,
table properties, file checksums, and, with `paranoid_file_checks`, a scan of
the new table against a hash of its entries. These are rejected because no
current caller needs them.

## Decision

Add `src/engine/build_table.{h,cc}`:

```cpp
Result<std::optional<FileMetadata>> BuildTable(FileSystem& file_system,
                                               const std::filesystem::path& directory,
                                               const InternalKeyComparator& comparator,
                                               const TableBuilderOptions& options,
                                               TableCache& table_cache,
                                               const MemTable& memtable,
                                               std::uint64_t number);
```

- An empty memtable returns nothing and touches no file.
- Otherwise `BuildTable` opens `TableFileName(directory, number)`, adds the
  memtable's entries in order, finishes the table, and finds it in the table
  cache with the finished size. It returns the number, the file size, and the
  first and last internal keys.
- The first error from the file system, the table builder, or the table cache
  is returned. `BuildTable` stops adding entries at the first error from
  `Add` and still calls `Finish`, which closes the file and returns that
  error. After the file is opened, any failure removes it once the builder
  has closed it; an error from the removal itself is ignored, because the
  number is not recorded anywhere and obsolete-file cleanup removes the file
  later.
- The table cache must read `directory` through the same file system with the
  same comparator. A fresh number is never in it, because only a successful
  build caches a table, so finding the table opens the new file.
- The comparator must order the memtable's keys as the memtable does, and the
  memtable must not change while its table is built.
- The number must be a fresh file number from the version set. The caller
  syncs the directory before a MANIFEST edit references the table, and keeps
  the number from obsolete-file cleanup until a version that lists the table
  is installed. If a MANIFEST edit that adds the table fails, the file stays
  until the database is reopened, as
  [ADR-0027](0027-version-set.md) requires.

## Explicitly deferred behavior

- Choosing a level above 0 for a flushed table, which belongs to the flush
  node.
- Building tables from iterators other than a memtable.
- Output validation beyond opening the table, table properties, and file
  checksums.

## Validation plan

Unit tests with an in-memory file system cover:

- A table built from a memtable with values and deletions: its metadata, its
  bytes against the table builder's output for the same entries and options,
  its entries read back through the table, its presence in the table cache,
  and the file operations, which include no directory sync.
- An empty memtable, which performs no file operation.
- A failure at every file operation, each returning the error and leaving no
  file behind, including a failed append while entries are added.
- A failed removal after a failure, which returns the original error and may
  leave the file.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

No differential helper is needed: the table bytes come from `TableBuilder`,
whose output matches LevelDB's byte for byte (ADR-0024), and the tests compare
`BuildTable`'s bytes with it.

## Consequences

- Recovery and flush share one tested way to turn a memtable into a durable,
  verified table.
- The DAG gains the `implement-table-build` node before recovery.

## References

- [Google LevelDB `BuildTable`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/builder.cc)
- [Google LevelDB `WriteLevel0Table`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [RocksDB `BuildTable`](https://github.com/facebook/rocksdb/blob/main/db/builder.cc)
