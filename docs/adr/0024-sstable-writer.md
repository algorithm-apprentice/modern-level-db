# ADR-0024: SSTable Writer

- Status: Accepted
- Date: 2026-09-24

## Context

Flush and compaction write sorted internal-key entries into immutable SSTable
files. A table consists of data blocks, an optional filter block, a metaindex
block, an index block, and a footer, as defined by
[ADR-0022](0022-sstable-block-format.md) and
[ADR-0023](0023-sstable-filter-blocks.md).

This implements only the `implement-sstable-writer` DAG node: building one
table into an open writable file. Choosing file names, deleting abandoned
files, syncing the database directory, recording the table in a version edit,
verifying the table by reading it, and splitting compaction output belong to
the flush, compaction, and version-set nodes.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Flush | Add a memtable's entries in order, finish the table durably, and learn its size |
| Compaction | Add merged entries in order, watch the file size to decide when to start another table, finish the table durably, and abandon it after an error |

Both callers write internal keys ordered by an `InternalKeyComparator`, and
both sync and close the file immediately after finishing it. No current caller
requires compression, tables of keys other than internal keys, changing
options while building, or table properties.

## Prior art

### Google LevelDB

Adopt LevelDB's table construction exactly, so that identical inputs produce
identical files:

- Entries enter a data block until its size estimate reaches `block_size`; the
  block is then written with its trailer.
- The index entry for a data block is added when the next key arrives, using
  `FindShortestSeparator(last key, next key)`, or at the end using
  `FindShortSuccessor(last key)`. Its value is the block handle. The index
  block uses restart interval 1.
- The filter block starts a range at offset 0 and at the offset after every
  data block, and receives every key.
- `Finish` writes the last data block, the filter block, a metaindex block
  that maps `filter.<policy name>` to the filter block's handle, the index
  block, and the footer.
- Defaults are a 4 KiB block size and restart interval 16.

Change:

- **Internal keys only.** Keys must be valid internal keys in strictly
  increasing order under the comparator; anything else is `InvalidArgument`.
  LevelDB only asserts the order and wraps its filter policy to strip the
  internal-key trailer. Here the writer passes user keys to the filter block
  directly, as ADR-0023 decided. LevelDB's compaction copies entries whose
  keys fail to parse into its output; the compaction node must handle such
  corrupt entries before they reach this writer, so byte equivalence covers
  valid internal keys.
- **Finishing is durable.** LevelDB's callers sync and close the file after
  `Finish`. Here `Finish` syncs and closes the file itself, so that no caller
  can reference an unsynced table.
- **Owned file and first-error state.** Like `WalWriter` in
  [ADR-0018](0018-wal-stream-io.md), the builder owns the file, and its first
  error makes every later `Add` and `Finish` return that error.
- **No flush per block.** LevelDB flushes the file after every data block. The
  bytes and their durability do not depend on it, so the writer relies on the
  file's buffering and on the final `Sync`.
- **Uncompressed.** Compression belongs to `implement-compression`.

### RocksDB

RocksDB's block-based table builder adds compression, properties blocks,
partitioned indexes and filters, parallel compression, and file checksums.
These are rejected because no current caller needs them.

## Decision

Add `src/table/table_builder.{h,cc}`:

```cpp
struct TableBuilderOptions {
  std::size_t block_size = 4096;
  std::uint32_t restart_interval = 16;
  std::optional<BloomFilterPolicy> filter_policy;
};

class TableBuilder {
 public:
  TableBuilder(std::unique_ptr<WritableFile> file, const InternalKeyComparator& comparator,
               const TableBuilderOptions& options);
  TableBuilder(std::unique_ptr<WritableFile> file, const InternalKeyComparator&& comparator,
               const TableBuilderOptions& options) = delete;

  Status Add(ByteView internal_key, ByteView value);
  Status Finish();

  std::uint64_t entry_count() const noexcept;
  std::uint64_t file_size() const noexcept;
};
```

- The comparator must outlive the builder; the deleted overload rejects a
  temporary comparator at compile time. The restart interval must be at least one. The public API
  validates user options, such as LevelDB's 1 KiB to 4 MiB block size range.
- `Add` returns `InvalidArgument` for a key that is not a valid internal key
  or does not follow the previous key, and propagates filter and block size
  errors and file errors.
- Any error from `Add` is kept: later `Add` calls return it without further
  writes, and `Finish` returns it after closing the file. An exception, which
  only allocation failure raises, leaves the builder unusable.
- `Finish` is a single terminal operation, as `WalWriter::Close` is. It writes
  the remaining blocks and the footer, syncs the file, and closes it; after an
  earlier error it writes nothing but still closes the file. It returns the
  first error. Every `Add` or `Finish` after it returns `InvalidArgument`.
- Destroying a builder without `Finish` abandons the table: the file's
  destructor closes it, and the caller deletes it.
- A null file makes every call return `InvalidArgument`.
- `file_size` counts the bytes appended so far: the written data blocks
  before `Finish`, and the whole table after it succeeds.
- A table without entries is valid and has an empty index block. Without a
  filter its metaindex block is empty; with one, it holds the entry for the
  five-byte empty filter block.

## Explicitly deferred behavior

- Compression, table properties, and statistics.
- File naming, deletion, directory sync, and verification by reading the
  table.
- Keys other than internal keys and changing options while building.

## Validation plan

Unit tests cover:

- Exact golden bytes for tables produced by LevelDB, with and without a
  filter, including the empty table.
- Reading every entry back through the block format, index, and filter
  primitives, across block sizes and restart intervals.
- Shortened index keys and the final successor key.
- Every rejected key, filter size error, file failure, and call after
  `Finish`, with the first error retained, the file closed, and no sync after
  a failure.
- `file_size` and `entry_count`.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md). The only exceptions are branches
  that forward block or filter size errors that only more than 4 GiB of block
  data or table offsets beyond 2 TiB can cause; they carry ADR-0019
  exclusion markers. Filter size errors that a large `bits_per_key` causes
  are tested.

A session-only differential helper builds random tables with unmodified
LevelDB and Modern LevelDB from the same entries and options and compares the
files byte for byte.

## Consequences

- Tables are byte-identical to uncompressed LevelDB tables built from the
  same entries and options.
- A finished table is durable; the caller still syncs the directory before
  recording the table in the MANIFEST.

## References

- [Google LevelDB table builder](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/table_builder.cc)
- [Google LevelDB flush](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/builder.cc)
- [Google LevelDB compaction output](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [RocksDB block-based table builder](https://github.com/facebook/rocksdb/blob/main/table/block_based/block_based_table_builder.cc)
