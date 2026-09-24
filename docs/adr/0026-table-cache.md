# ADR-0026: Table Cache

- Status: Accepted
- Date: 2026-09-24

## Context

Reads, compactions, and the checks that flushes and compactions run on new
tables open SSTables by file number. Opening a table reads its footer, index
block, and filter block, as described in [ADR-0025](0025-sstable-reader.md),
so the database keeps a limited number of recently used tables open and
shares them among concurrent readers.

This implements only the `implement-table-cache` DAG node: opening the table
of a file number and keeping open tables in an LRU cache. Choosing the
files to read, iterating across tables, and deleting obsolete files belong to
the read-path, compaction, and engine nodes.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Read path | Find the open table of each file that a lookup or iterator visits, and keep it open while it is in use |
| Compaction | Find input tables, which it reads without filling the block cache |
| Flush and compaction | Open a new table once to check that it is readable |
| Obsolete-file cleanup | Stop caching a table before its file is deleted |
| Database | Limit the number of tables kept open |

No current caller requires legacy `.sst` table names, lookups that must not
perform I/O, table properties, approximate offsets, or keeping every table
open.

## Prior art

### Google LevelDB

Adopt LevelDB's table cache:

- The cache key is the file number as fixed64, and each table has charge one,
  so the capacity counts tables, rounded up per shard. LevelDB's database
  uses `max_open_files` minus ten reserved files.
- A miss opens `<number>.ldb` in the database directory, opens the table with
  the file size that the MANIFEST records, and caches it.
- Errors are not cached, so the next lookup retries a transient failure or a
  repaired file.
- Evicting a file number erases its entry; tables in use stay open until they
  are released.
- Concurrent misses for the same file may each open it; the last insertion
  replaces the earlier entry.

Change:

- **Handles instead of wrappers.** LevelDB returns an iterator that releases
  the cache handle when it is destroyed, and a `Get` that passes the first
  entry at or after the key to a callback. Here `Find` returns the cache
  handle, which keeps the table open, and callers use the table's own `Get`
  and `Iterator` while they hold it.
- **No legacy `.sst` fallback.** LevelDB tries `<number>.sst` whenever opening
  `<number>.ldb` fails. [ADR-0020](0020-database-file-names.md) defers `.sst`
  names, which only LevelDB releases before 1.14 wrote, so only
  `<number>.ldb` is opened.
- **Engine layer.** The architecture listed the table cache in the `table`
  layer, but it names files with the `metadata` layer's file names, which is
  a higher layer, and only engine components use it. It lives in
  `src/engine/`, matching LevelDB and RocksDB, which keep it in `db/`. The
  read path, flush, compaction, and database state depend on it within the
  engine layer in DAG order, and the architecture now states that modules may
  depend on modules of their own layer in that order.

### RocksDB

RocksDB's table cache adds striped locks so that concurrent misses load a
table once, `no_io` lookups that return `Incomplete` for a table that is not
cached, readers pinned for the life of a file when every file may stay open,
a row cache, and table properties and approximate sizes. It falls back to the
LevelDB name only when the first name is not found. These are rejected
because no current caller needs them; duplicate opens on concurrent misses
are rare and harmless.

## Decision

Add `src/engine/table_cache.{h,cc}`:

```cpp
class TableCache {
 public:
  using Handle = ShardedLruCache<Table>::Handle;

  TableCache(FileSystem& file_system, std::filesystem::path directory,
             const InternalKeyComparator& comparator, const TableOptions& options,
             std::size_t capacity);
  TableCache(FileSystem& file_system, std::filesystem::path directory,
             const InternalKeyComparator&& comparator, const TableOptions& options,
             std::size_t capacity) = delete;

  Result<Handle> Find(std::uint64_t file_number, std::uint64_t file_size);
  void Evict(std::uint64_t file_number);
};
```

- `Find` returns the cached table of the file number. On a miss it opens
  `TableFileName(directory, file_number)` through the file system, opens the
  table with the file size, the comparator, and the options, and caches it.
  The file number must be nonzero, as `TableFileName` requires, and the size
  must be the file's size; a cached table ignores the size.
- `Find` returns the file system's and `Table::Open`'s errors unchanged and
  caches nothing when it fails.
- A handle keeps its table open, including after `Evict` or after a
  concurrent miss replaces its cache entry; the capacity never closes a table
  that a handle holds. Handles must be released before the table cache is
  destroyed, as [ADR-0014](0014-sharded-lru-cache.md) requires of cache
  handles. The file system, the comparator, and the options' block cache must
  outlive the table cache; the deleted overload rejects a temporary
  comparator at compile time.
- `Evict` erases the cached table of the file number. It does not wait for
  handles or for a `Find` that is opening the file, so it is not by itself a
  barrier before deletion. As in LevelDB, the engine deletes a table file only
  when no live version and no pending output refers to it, so no `Find` for
  the file is in progress or can start and no handle holds it; it then calls
  `Evict` before deleting the file. Every path that deletes a table file
  follows this order.
- Each table has charge one. As in LevelDB and ADR-0014, each of the cache's
  16 shards has a capacity of `ceil(capacity / 16)` tables, which it enforces
  only when it caches a table, by closing its least recently used tables that
  no handle holds. The capacity is therefore a target rather than a limit on
  open files: the shards together may keep up to 15 more tables than the
  capacity, pinned tables stay open beyond it, and tables released after
  their shard last cached a table stay open until it caches another. A
  capacity of zero caches nothing.
- An insertion cannot overflow the cache's charge accounting, which would
  require `SIZE_MAX` tables in one shard; `Find` returns the error if it did.
- `Find` and `Evict` are safe for concurrent calls. The table cache is neither
  copyable nor movable.

## Explicitly deferred behavior

- Legacy `.sst` table names, as ADR-0020 decided.
- Lookups that must not perform I/O, tables pinned for the life of a file,
  table properties, and approximate offsets.
- Loading a table once for concurrent misses.

## Validation plan

Unit tests with an in-memory file system cover:

- Opening the table of a file number from its canonical name in the
  directory, and reading it through the handle with the configured filter and
  block cache.
- Cache hits without reopening, distinct tables for distinct file numbers,
  eviction by capacity, and handles that stay usable after `Evict`, while
  their shard evicts other tables, and with a capacity of zero.
- Errors that are not cached: a missing file, a failing open, and a corrupt
  table, each followed by a successful `Find` once the file is repaired.
- Concurrent `Find` and `Evict` calls.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

A session-only differential helper opens tables that unmodified LevelDB wrote
through both table caches with small capacities and random evictions, and
compares lookups, iteration, and when each cache opens files.

## Consequences

- Engine components open tables by file number and keep recently used tables
  open with a capacity that is a target rather than a limit on open files,
  and the next lookup retries a failed open.
- Deleting a table file safely depends on the engine's live-file tracking;
  `Evict` only stops caching the table.
- Databases with `.sst` table names, which LevelDB releases before 1.14 wrote,
  cannot be read until the deferred fallback exists.
- The architecture lists the table cache in the `engine` layer, and the flush,
  compaction, and database-engine nodes depend on this node in the DAG.

## References

- [Google LevelDB table cache](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/table_cache.cc)
- [Google LevelDB table cache sizing](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [RocksDB table cache](https://github.com/facebook/rocksdb/blob/main/db/table_cache.cc)
