# ADR-0030: Point Reads

- Status: Accepted
- Date: 2026-09-24

## Context

A point read finds the newest version of a user key that a snapshot can see.
It looks in the mutable memtable, then in the immutable memtable, and then in
the tables of the current version ([ADR-0027](0027-version-set.md)), which it
opens through the table cache ([ADR-0026](0026-table-cache.md)).

The DAG's `implement-read-path` node combined point reads with merged
iteration. Merged iteration is also the input of compaction, while point reads
serve only the database's `Get`, so this ADR splits the node: it implements
point reads, and the new `implement-iterators` node, which compaction and the
database engine depend on, implements merged iteration.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Database `Get` | Read a user key at a snapshot sequence from the memtables and version it holds, telling a missing or deleted key apart from an error |

No current caller requires seek statistics, which trigger compactions and
belong to the compaction node, reading several keys at once, or partial
results.

## Prior art

### Google LevelDB

Adopt `DBImpl::Get` and `Version::Get`:

- The lookup key is the user key with the snapshot sequence, so the first
  entry at or after it with the same user key is the newest visible version.
- The memtable is searched, then the immutable memtable, then the version.
  The first source with an entry for the user key decides: a value is
  returned, and a deletion means the key is absent.
- In the version, the level-0 files whose user-key range contains the user
  key are searched from newest to oldest, which is from the largest file
  number down. Each deeper level holds at most one candidate: the first file
  whose largest key is not before the lookup key, if its smallest user key is
  not after the user key.
- A table error ends the read.

Change:

- **Typed results.** LevelDB returns `NotFound` both for an absent key and, for
  example, for a table file that the file system cannot find. Here an absent
  or deleted key is an empty result, and every error is an error.
- **No seek statistics.** LevelDB records the first file that a read searched
  without deciding it, which can trigger a compaction. Compaction scores and
  seek statistics belong to the compaction node, as ADR-0027 decided.

### RocksDB

RocksDB's point reads add merge operands, range deletions, a row cache,
batched `MultiGet`, and fractional cascading between levels through its
`FilePicker` and `FileIndexer`. These are rejected because no current caller
needs them.

## Decision

Add `src/engine/lookup.{h,cc}`:

```cpp
Result<std::optional<std::vector<std::byte>>> LookupValue(
    const MemTable& memtable, const MemTable* immutable, const Version& version,
    TableCache& table_cache, const InternalKeyComparator& comparator, const LookupKey& key,
    const TableReadOptions& options = {});
```

- `LookupValue` returns the value of the newest entry of the key's user key
  whose sequence is at most the key's sequence, searching as LevelDB does, or
  nothing if that entry is a deletion or no source has one. The immutable
  memtable is optional.
- The table cache's and the tables' errors are returned unchanged, including
  the file system's `NotFound` for a missing table file. Tables after the
  deciding source are not opened.
- The memtables, the version's files, and the table cache must use the
  comparator. The function only reads, so concurrent calls are safe as long
  as the memtables' single writer does not add to them concurrently with a
  read that expects to see the addition.
- The caller chooses the lookup sequence and captures the memtables and the
  version together, as LevelDB does under its mutex, and keeps all three
  alive until the call returns. Otherwise a memtable could be released while
  it is read, or a read could see an immutable memtable's entries neither in
  the memtable nor in the version that replaced it. The database engine
  implements and tests this capture.
- A deleted overload rejects a temporary comparator.

## Explicitly deferred behavior

- Seek statistics and read sampling, which belong to the compaction node.
- Merged iteration, which belongs to the `implement-iterators` node.
- Reading several keys at once.

## Validation plan

Unit tests cover:

- Values and deletions in the memtable and the immutable memtable, which
  decide before the version, and a missing immutable memtable.
- Entries newer than the lookup sequence, which are invisible in every source.
- Level-0 files searched from newest to oldest, only those whose range
  contains the user key, with deletions and invisible versions.
- One candidate per deeper level, including keys before, between, and after
  files, a user key whose versions span two files, and deletions that hide
  deeper values.
- Errors from missing and damaged tables, and tables that are never opened
  because an earlier source decided.
- A user comparator that orders keys in reverse, for level-0 candidates and a
  boundary user key in a deeper level.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

A session-only differential helper has unmodified LevelDB write uncompressed
databases whose tables span several levels and hold several versions of keys,
recovers their versions with both implementations, and compares
`LookupValue` with LevelDB's `Version::Get` for random keys and sequences.

## Consequences

- The database's `Get` reads its sources with one call and can tell an absent
  key from a failure.
- The compaction node, which now depends on this node, adds seek statistics
  to point reads.

## References

- [Google LevelDB `DBImpl::Get`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [Google LevelDB `Version::Get`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [RocksDB `FilePicker`](https://github.com/facebook/rocksdb/blob/main/db/version_set.cc)
