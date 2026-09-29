# ADR-0025: SSTable Reader

- Status: Accepted

[ADR-0039](0039-sstable-block-compression.md) extends this reader to decode
Snappy and Zstd blocks. Cache charges count decoded bytes rather than the
stored compressed size; malformed compressed input returns `Corruption`.

[ADR-0049](0049-leveldb-style-point-read-baseline.md) defines the reference
controls and diagnostic evidence used to change data-block validation. Data
blocks now retain complete structural validation while trusting the writer's
key order; index and metaindex blocks retain full order validation.

[ADR-0050](0050-posix-mmap-table-reads.md) adds direct stored-block decoding
from an optional mapped file view. [ADR-0056](0056-leveldb-table-mmap-parity.md)
preserves borrowed mapped-uncompressed contents through `Block` and the filter
reader; borrowed data blocks never enter the block cache.

[ADR-0051](0051-trusted-internal-key-comparison.md) designs one-time index-key
parsing and a minimum data-key length boundary before trusted block seeks.
[ADR-0055](0055-leveldb-block-iterator-parity.md) replaces eager block scans
with lazy checked decoding and completes that trusted boundary.
[ADR-0057](0057-leveldb-version-output-parity.md) replaces the owning point
lookup result with caller-buffer output and an explicit lookup kind.
- Date: 2026-09-24

## Context

Reads and compactions need point lookups and ordered iteration over immutable
SSTables written as described in [ADR-0022](0022-sstable-block-format.md),
[ADR-0023](0023-sstable-filter-blocks.md), and
[ADR-0024](0024-sstable-writer.md). Data blocks are read on demand and may be
shared through a block cache.

This implements only the `implement-sstable-reader` DAG node: opening one table
file, point lookups, iteration, and the block cache. Opening files by number,
caching open tables, merging iterators across tables, and approximate offsets
belong to later nodes.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Table cache | Open a table from a random-access file and its size, and share it among concurrent readers |
| Read path | Find the newest version of a user key visible at a sequence number, using the filter to skip data blocks |
| Read path and compaction | Iterate in both directions and seek within one table |
| Compaction | Read blocks without filling the block cache |

No current caller requires approximate offsets, reading compressed blocks,
custom filter policies, or reading tables of keys other than internal keys.

## Prior art

### Google LevelDB

Adopt LevelDB's read path:

- `Open` reads the footer and the index block. If a filter policy is
  configured, it reads the metaindex block and, when the metaindex maps
  `filter.<policy name>` to a handle, the filter block.
- A point lookup seeks the index to the first index key not less than the
  target, skips the data block if the filter rules the key out, and otherwise
  seeks that data block. The index keys guarantee that the first version of
  the user key not newer than the target is in that block, if the table has
  one.
- Iteration is a two-level iterator: the index block chooses data blocks, and
  data block iterators produce entries.
- The block cache key is the table's cache ID followed by the block offset,
  both fixed64; its charge is the block size. Reads that must not fill the
  cache still use cached blocks.

Change:

- **Validate what is read.** LevelDB verifies checksums only when asked,
  ignores unreadable metaindex and filter blocks, and allows bytes after a
  handle in index values. Here every block's checksum is verified, the
  metaindex and filter blocks must be valid, each index value must be
  exactly one handle of a block that lies before the footer, and the indexed
  blocks must follow one another in file order without overlapping, so a
  block's offset identifies it in the block cache. Every problem is
  `Corruption`, found when `Open` reads the block or when a data block is
  first read. ADR-0049 narrows this promise for data blocks: structurally
  valid but unordered entries are not diagnosed, because their order is an
  invariant of conforming table writers.
- **Non-empty data blocks.** LevelDB skips data blocks without entries, which
  its writer never produces. Here such a block is `Corruption`, so an iterator
  that leaves a data block needs only the adjacent block.
- **Complete reads.** A random-access read may return fewer bytes than asked,
  as [ADR-0011](0011-filesystem-contracts.md) allows; the reader repeats reads
  until the block is complete and reports `Corruption` if the file ends first.
- **Typed lookup.** LevelDB's lookup hands the first entry at or after the
  target to a callback, which compares user keys. Here `Get` takes a
  `LookupKey`, compares user keys with the user comparator, and returns the
  value kind and an owned copy of the value, or nothing.
- **Status per move.** LevelDB iterators keep an error for `status()`. Here
  each positioning call returns `Status` and leaves the iterator invalid on
  failure, following the codebase's explicit results.

### RocksDB

RocksDB's block-based table reader adds partitioned indexes and filters,
prefetching, pinned iterators, a separate cache for index and filter blocks,
and table properties. These are rejected because no current caller needs
them.

## Decision

Add `src/table/table.{h,cc}`:

```cpp
using BlockCache = ShardedLruCache<Block>;

struct TableOptions {
  std::optional<BloomFilterPolicy> filter_policy;
  BlockCache* block_cache = nullptr;
};

struct TableReadOptions {
  bool fill_cache = true;
};

enum class TableLookupKind {
  Missing,
  Value,
  Deletion,
};

class Table {
 public:
  static Result<std::unique_ptr<Table>> Open(std::unique_ptr<RandomAccessFile> file,
                                             std::uint64_t file_size,
                                             const InternalKeyComparator& comparator,
                                             const TableOptions& options);
  static Result<std::unique_ptr<Table>> Open(std::unique_ptr<RandomAccessFile> file,
                                             std::uint64_t file_size,
                                             const InternalKeyComparator&& comparator,
                                             const TableOptions& options) = delete;

  Result<TableLookupKind> Get(const LookupKey& key,
                              std::vector<std::byte>& value,
                              const TableReadOptions& options = {}) const;

  class Iterator {
   public:
    explicit Iterator(const Table& table, const TableReadOptions& options = {});
    bool valid() const noexcept;
    ByteView key() const noexcept;
    ByteView value() const noexcept;
    Status SeekToFirst();
    Status SeekToLast();
    Status Seek(ByteView target);
    Status Next();
    Status Prev();
  };
};
```

- `Open` returns `Corruption` for a file shorter than the footer, a malformed
  footer, index, metaindex, or filter block, or a block outside the file or
  larger than a `std::size_t` can count, `NotSupported` for a compressed
  block, and the file's errors. A null file is `InvalidArgument`.
- The table owns the file, index block, and filter reader. It is neither
  copyable nor movable, and its const members are safe for concurrent calls.
  The comparator and the block cache must outlive the table; the deleted
  overload rejects a temporary comparator at compile time.
- `InternalKeyComparator` gains `const Comparator& user_comparator() const
  noexcept` for the user-key comparison.
- The filter is used only if the options name the policy the table was built
  with; otherwise lookups read the data block.
- Filters hash user-key bytes, so a filter policy requires a user comparator
  that considers two user keys equal only when their bytes are equal; any
  other comparator could make a filter hide a key. LevelDB documents the same
  requirement. The writer's and reader's options state it as a precondition,
  and the public API must enforce it when it accepts comparators and
  filters.
- `Get` returns the kind of the first entry not less than the lookup key if it
  has the lookup's user key, and `Missing` otherwise. A value is copied
  directly into the caller's reusable vector; missing and deletion leave that
  vector unchanged. The internal-key comparator orders keys that are not valid
  internal keys before every valid key, so a lookup, whose key is valid, never
  lands on one.
- An iterator reads a table that must outlive it; it is neither copyable nor
  movable. A new iterator is not positioned. `key` and `value` require a valid
  position and remain valid until the iterator moves. A failed positioning
  call leaves it invalid.
- Data blocks are read through the block cache when one is configured.
  `fill_cache = false` still uses cached blocks but does not insert new ones.
  A block whose charge the cache cannot account is used without caching it.
- Every block uses minimal restart-region construction. Data entries are
  checked lazily as iterator moves reach them. Index and metaindex blocks
  receive one complete physical entry/restart-topology validation at table
  open, and production table seeks use the checked trusted internal-key
  comparator boundary.
- Stored-block reads prefer an exact stable file view. Compressed bytes are
  checksummed and decompressed directly from it into owned cacheable storage;
  uncompressed mapped bytes remain borrowed and noncacheable. Copied
  uncompressed bytes reuse their owning allocation and remain cacheable. When
  no view exists, `ReadExactly` retains complete-read and truncation behavior.
- A block iterator borrows its block, so an iterator keeps the current block's
  intrusive cache handle or an uncached owned `Block` object while positioned.
  An uncached mapped block owns only metadata and borrows bytes from the table,
  which outlives it. `Get` keeps the reference through key parsing and value
  copying.

## Explicitly deferred behavior

- Approximate offsets, table properties, and prefetching.
- Compressed blocks, which belong to `implement-compression`.
- Opening tables by file number and caching open tables, which belong to
  `implement-table-cache`.
- Checking that data block keys agree with their index keys.

## Validation plan

Unit tests cover:

- Reading tables written by `TableBuilder`, including LevelDB's golden tables
  and empty tables, by lookup and by iteration against an ordered model across
  block sizes, restart intervals, and filters.
- Lookups of every version of a user key at different sequence numbers,
  deletions, missing keys, and keys past the end.
- Filter lookups that skip data block reads, and block cache hits,
  insertions, insertions that overflow the cache's charge accounting, and
  `fill_cache = false`.
- Iterators and lookups over a cache without capacity and over blocks evicted
  while an iterator is positioned in them.
- Every `Open` rejection, short and failed reads, and corrupt and empty data
  blocks found by lookups and iterators.
- Concurrent lookups.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

A session-only differential helper opens random tables written by unmodified
LevelDB and compares lookups and iterator positions with LevelDB's table
reader.

## Consequences

- Modern LevelDB reads uncompressed LevelDB tables, and a damaged table fails
  loudly instead of being read without its filter or with skipped blocks.
- Table iterators have explicit errors; merging iterators must propagate them.

## References

- [Google LevelDB table reader](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/table.cc)
- [Google LevelDB two-level iterator](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/two_level_iterator.cc)
- [Google LevelDB lookup callback](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [RocksDB block-based table reader](https://github.com/facebook/rocksdb/blob/main/table/block_based/block_based_table_reader.cc)
