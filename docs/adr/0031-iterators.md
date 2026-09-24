# ADR-0031: Iterators

- Status: Accepted
- Date: 2026-09-24

## Context

Compaction reads the entries of its input tables in internal-key order, and a
database iterator reads the user keys that a snapshot sees in the memtables and
the tables of a version. Both merge several sorted sources: memtables
([ADR-0017](0017-arena-backed-memtable.md)), tables
([ADR-0025](0025-sstable-reader.md)) opened through the table cache
([ADR-0026](0026-table-cache.md)), and the levels of a version
([ADR-0027](0027-version-set.md)).

This implements the `implement-iterators` DAG node, which
[ADR-0030](0030-point-reads.md) split from the read path. Choosing a snapshot,
capturing the memtables and the version, and choosing compaction inputs remain
with the engine and compaction nodes.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Compaction | Read the entries of its input files in internal-key order without filling the block cache: each level-0 file separately and the files of a deeper level as one sequence |
| Database iterators | Read the user keys and values that a snapshot sees in the memtables and the current version, in both directions and from seeks |
| Obsolete-file cleanup | Treat the files of a version that an iterator reads as live |

No current caller requires iteration bounds, prefix seeks, tailing iterators,
values that stay valid after a move, or read sampling, which triggers seek
compactions and belongs to the compaction node as ADR-0030 decided.

## Prior art

### Google LevelDB

Adopt LevelDB's iterators:

- An internal iterator reads a memtable, a table, or a level. A level
  iterator reads the files of a level in order and opens each table through
  the table cache when it reaches the file; a seek starts at the first file
  whose largest key is not before the target.
- A merging iterator yields the smallest current key of its children, found
  by a linear scan, and the largest when it moves backward. When it changes
  direction, it positions every other child after or before the current key.
- A database iterator (`DBIter`) reads a merged internal iterator at a
  snapshot sequence. Moving forward, it skips entries newer than the
  snapshot, older versions of the key it has returned, and deleted keys.
  Moving backward, it passes over each user key's entries and keeps the
  newest visible one, whose key and value it copies. A seek finds the first
  user key at or after the target.
- The database's internal iterator merges the memtable, the immutable
  memtable, each level-0 file, and each deeper level.
- An iterator keeps its memtables, tables, and version alive.

Change:

- **Status per move.** LevelDB iterators keep an error for `status()`, and
  `DBIter` skips an entry whose key is not an internal key while recording
  the error. Here every positioning call returns `Status`, a failure leaves
  the iterator invalid, and a key that is not an internal key is
  `Corruption`, as ADR-0025 decided for tables.
- **Tables open on first use.** LevelDB opens every level-0 table when it
  creates a database iterator and turns a failure into an iterator that only
  reports it. Here each level-0 file is a level iterator over one file, so
  creating an iterator performs no I/O and cannot fail; errors from opening a
  table are returned by the move that reaches it.
- **Empty tables are corrupt.** LevelDB skips tables without entries, which
  none of its writers installs in a version: flushes and repair keep only
  tables with entries, and compaction opens an output only for an entry. A
  version lists every table with its smallest and largest keys, so here a
  table without entries is `Corruption`, and leaving a file needs only the
  adjacent one. A seek that passes the last entry of a table whose recorded
  largest key is later, which LevelDB also tolerates, continues at the first
  entry of the next file; the level iterator tells it from an empty table by
  positioning the same table at its first entry.
- **Shared ownership.** LevelDB reference-counts memtables and versions and
  registers cleanup functions on iterators. Here an iterator holds
  `std::shared_ptr<const MemTable>` and `std::shared_ptr<const Version>`, so
  the engine shares memtables the same way; a held version also keeps its
  files in `VersionSet::LiveFiles()`.

LevelDB's `IteratorWrapper`, which caches a child's key to avoid virtual
calls, is not adopted; the children's accessors are already cheap.

### RocksDB

RocksDB adds a heap-based merging iterator, iteration bounds, prefix seeks,
tailing and pinned iterators, range deletions, and a reseek after a number of
hidden entries in a row. These are rejected because no current caller needs
them.

## Decision

Add `src/engine/internal_iterator.h`, `src/engine/iterators.{h,cc}`, and
`src/engine/db_iterator.{h,cc}`:

```cpp
class InternalIterator {
 public:
  InternalIterator(const InternalIterator&) = delete;
  InternalIterator& operator=(const InternalIterator&) = delete;
  InternalIterator(InternalIterator&&) = delete;
  InternalIterator& operator=(InternalIterator&&) = delete;
  virtual ~InternalIterator() = default;
  virtual bool valid() const noexcept = 0;
  virtual ByteView key() const noexcept = 0;
  virtual ByteView value() const noexcept = 0;
  virtual Status SeekToFirst() = 0;
  virtual Status SeekToLast() = 0;
  virtual Status Seek(ByteView target) = 0;
  virtual Status Next() = 0;
  virtual Status Prev() = 0;
};

std::unique_ptr<InternalIterator> NewMemTableIterator(std::shared_ptr<const MemTable> memtable);
std::unique_ptr<InternalIterator> NewLevelIterator(std::shared_ptr<const Version> version,
                                                   std::span<const Version::File> files,
                                                   TableCache& table_cache,
                                                   const InternalKeyComparator& comparator,
                                                   const TableReadOptions& options);
std::unique_ptr<InternalIterator> NewMergingIterator(
    const InternalKeyComparator& comparator,
    std::vector<std::unique_ptr<InternalIterator>> children);
std::unique_ptr<InternalIterator> NewInternalIterator(std::shared_ptr<const MemTable> memtable,
                                                      std::shared_ptr<const MemTable> immutable,
                                                      std::shared_ptr<const Version> version,
                                                      TableCache& table_cache,
                                                      const InternalKeyComparator& comparator,
                                                      const TableReadOptions& options);

class DbIterator final {
 public:
  DbIterator(std::unique_ptr<InternalIterator> internal, const Comparator& user_comparator,
             SequenceNumber sequence);
  DbIterator(std::unique_ptr<InternalIterator> internal, const Comparator&& user_comparator,
             SequenceNumber sequence) = delete;
  DbIterator(const DbIterator&) = delete;
  DbIterator& operator=(const DbIterator&) = delete;
  DbIterator(DbIterator&&) = delete;
  DbIterator& operator=(DbIterator&&) = delete;
  bool valid() const noexcept;
  ByteView key() const noexcept;
  ByteView value() const noexcept;
  Status SeekToFirst();
  Status SeekToLast();
  Status Seek(ByteView user_key);
  Status Next();
  Status Prev();
};
```

- An internal iterator yields internal keys in the comparator's order. A new
  iterator is not positioned. `Seek` finds the first entry at or after the
  target. `key` and `value` require a valid position and remain valid until
  the iterator moves; `Next` and `Prev` require a valid position. A failed
  move leaves the iterator invalid, and the next seek starts over.
- A level iterator reads files that are sorted by key and do not overlap,
  such as a level of the version or a run of its files; the version must hold
  the files. When a seek stays in the file that is already open, the table is
  not looked up again.
- Among equal keys, the merging iterator yields the first child's entry
  first when it moves forward and the last child's first when it moves
  backward, as LevelDB's does, so that moving backward retraces moving
  forward. It may have no children.
- `NewInternalIterator` merges the memtable, the immutable memtable if there
  is one, one level iterator for each level-0 file, and one for each deeper
  level with files.
- A database iterator yields each user key whose newest entry at or before
  the sequence is a value, with that value, in the user comparator's order.
  `key` returns the user key.
- As in LevelDB, while a database iterator moves forward, its internal
  iterator is at the entry that it yields. While it moves backward, it holds
  copies of the key and value that it yields, and its internal iterator is at
  the last entry before that user key's entries, or past the beginning when
  there is none. Changing direction to forward then seeks the internal
  iterator to its first entry. Moving backward past the first user key, like
  moving forward past the last, leaves the database iterator invalid without
  an error, and a failed internal move leaves it invalid and discards what it
  held.
- Every function that keeps a comparator has a deleted overload for a
  temporary one.
- The table cache and the comparators must outlive the iterators. An iterator
  is used by one thread at a time and is neither copyable nor movable;
  several iterators may read the same sources concurrently, and the
  memtable's single writer may add entries that iterators may or may not see.

## Explicitly deferred behavior

- Read sampling for seek compactions, which belongs to the compaction node.
- Iteration bounds, prefix seeks, tailing iterators, and reseeks after many
  hidden entries.

## Validation plan

Unit tests cover:

- The merging iterator over scripted children: order, equal keys in both
  directions and across direction changes, direction changes at every
  position against a model, no children and empty children, and a failure at
  every child move.
- The level iterator over tables in an in-memory file system: both directions
  across files, seeks before, inside, between, and after files, a seek past a
  table's last entry and an empty table, both without another table cache
  lookup, the reuse of an open table, failures to open and read tables, and
  the version it holds.
- The memtable iterator and `NewInternalIterator`, which merges every source.
- The database iterator over scripted internal iterators against an ordered
  model with random entries, deletions, snapshots, and operations in both
  directions; changing direction at both ends; keys that are not internal
  keys; and a failure at every internal move, including one after a backward
  move has copied its key and value.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

A session-only differential helper has unmodified LevelDB write uncompressed
databases across several levels while holding snapshots, which it keeps
until the databases close, so the files keep every version that the
snapshots see. It recovers copies with `RecoverDatabase` and compares
database iterators at every snapshot sequence with a model of the writes,
and at the last sequence also with LevelDB's own iterator.

## Consequences

- Compaction and database iterators share one set of iterators with explicit
  errors.
- The write path shares memtables through `std::shared_ptr`.
- Creating a database iterator performs no I/O.

## References

- [Google LevelDB merging iterator](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/merger.cc)
- [Google LevelDB two-level iterator](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/two_level_iterator.cc)
- [Google LevelDB `DBIter`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_iter.cc)
- [Google LevelDB `Version::AddIterators`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [RocksDB `DBIter`](https://github.com/facebook/rocksdb/blob/main/db/db_iter.cc)
