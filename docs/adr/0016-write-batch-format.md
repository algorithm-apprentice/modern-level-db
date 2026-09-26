# ADR-0016: Write Batch Format and Reader

- Status: Accepted
- Date: 2026-09-23

## Context

A write batch groups ordered Put/Delete operations that must be applied
atomically and persisted as one WAL logical record. Recovery and MemTable
insertion need to parse the same payload without copying every key and value.

This implements only the `implement-write-batch` DAG node. Public DB API
wrappers, writer grouping, WAL I/O, MemTable insertion, and recovery remain
separate future nodes.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Public write batch wrapper | Build ordered Put/Delete operations |
| DB writer queue | Set starting sequence, append compatible batches, inspect encoded size |
| WAL writer | Borrow the complete encoded payload |
| Recovery | Validate a WAL payload and iterate operations in order |
| MemTable insertion | Receive kind, key, value, and per-entry sequence without copies |

No current caller requires column families, merge operands, range deletion,
single delete, log data, savepoints, indexing, protection bytes, or max-size
policy.

## Prior art and adopted decisions

### Google LevelDB

Adopt the format exactly:

```text
batch :=
  sequence fixed64 little-endian
  count    fixed32 little-endian
  record[count]

record :=
  Value-tag    varint32-key-length key varint32-value-length value
  Deletion-tag varint32-key-length key
```

The tags reuse the persistent `ValueKind` bytes:

```text
Deletion = 0
Value    = 1
```

Operations retain insertion order. Appending a source batch copies only its
record bytes, adds its count, and preserves the destination sequence.

### RocksDB

RocksDB retains the base format but adds column families, merge, range delete,
savepoints, indexed batches, log data, timestamps, and protection metadata.
These extensions are rejected because current callers require only original
LevelDB Put/Delete batches.

## Decision

### Persistent constants

```cpp
inline constexpr std::size_t WriteBatchHeaderSize = 12;
```

The first eight bytes are the starting sequence, followed by a four-byte
operation count.

### Owning batch

```cpp
class EncodedWriteBatch final {
 public:
  EncodedWriteBatch();

  Status Put(ByteView key, ByteView value);
  Status Delete(ByteView key);
  Status Append(const EncodedWriteBatch& source);
  Status SetSequence(SequenceNumber sequence);
  void Clear() noexcept;

  SequenceNumber sequence() const noexcept;
  std::uint32_t count() const noexcept;
  ByteView encoded() const noexcept;
};
```

`EncodedWriteBatch` owns one `std::vector<std::byte>` that always contains a valid
12-byte header followed by validated records. It is copyable and movable.
There is no invalid/default-empty representation: a default batch has sequence
zero, count zero, and exactly 12 header bytes.

`Put` and `Delete` copy caller bytes into the representation and increment the
fixed32 count. Empty keys and empty values are valid. A key or value larger than
`uint32_t` length, count overflow, or sequence-range overflow returns
`InvalidArgument` and leaves the batch unchanged.

Every mutator preserves the valid-representation invariant on validation or
allocation failure. `Put` and `Delete` first encode the complete record in
temporary storage, which also supports key/value views that alias the batch's
current `encoded()` storage. They then append the staged record in one vector
operation and update the header count last.

`SetSequence` accepts zero and rejects values above `MaxSequenceNumber`.
For a non-empty batch it also rejects a starting sequence whose final operation
would exceed `MaxSequenceNumber`.

`Clear` retains vector capacity, resets sequence/count to zero, and truncates to
the header without allocation.

`Append`:

- Validates destination count and final sequence range before mutation.
- Appends source records but not its header.
- Leaves destination sequence unchanged.
- Commits all source record bytes in one vector insertion and updates the count
  last.
- Supports self-append by staging the source record range before mutation.
- Leaves the destination unchanged on validation or allocation failure.

Move construction and move assignment are explicit: the destination receives
the representation and the source becomes the canonical zero-sequence,
zero-count 12-byte batch. If preparing that replacement header fails, neither
existing object changes. This preserves the class invariant for moved-from
objects and keeps `Clear()` non-throwing.

Any non-const operation may invalidate views previously returned by
`encoded()`. A reader borrowing a batch must not outlive or overlap mutation of
that batch.

### Validated borrowed reader

```cpp
struct WriteBatchEntry {
  SequenceNumber sequence;
  ValueKind kind;
  ByteView key;
  ByteView value;
};

class WriteBatchReader final {
 public:
  static Result<WriteBatchReader> Open(ByteView encoded);

  SequenceNumber sequence() const noexcept;
  std::uint32_t count() const noexcept;
  std::optional<WriteBatchEntry> Next() noexcept;
};
```

The reader borrows the complete encoded batch. Its views remain valid only
while that storage is alive and unchanged. The reader is non-copyable and
movable.

`Open` validates the complete representation before returning:

- At least 12 header bytes exist.
- The starting sequence is at most `MaxSequenceNumber`, and the final assigned
  sequence implied by count does not overflow that limit.
- Every record tag is `Deletion` or `Value`.
- Every varint32 length and referenced byte range is complete.
- Parsed record count exactly matches the header.
- No trailing bytes remain beyond the counted records.

Persistent malformed input returns `Corruption`. Validation performs no
per-entry key/value allocation. To preserve original LevelDB behavior,
non-canonical varints are accepted, including discarded payload bits beyond
bit 31 in a terminating fifth byte. A continuation bit after the fifth byte
remains corruption.

After successful `Open`, `Next` cannot return a format error and therefore
returns only `optional`. It returns entries in encoded order with sequences
`batch.sequence() + index`. A deletion entry has an empty value view; an empty
Put value is distinguished by kind.

### Encoding helpers

Use the existing fixed32/fixed64 and varint32 length-prefix primitives. No
second binary-coding implementation is added.

## Explicitly deferred behavior

- Applying entries to MemTable.
- Public API naming or exception adapters.
- Writer-group temporary batches and batching limits.
- Column families and operation kinds beyond Put/Delete.
- Savepoints, rollback, indexed reads, and transaction conflict tracking.
- User-configurable maximum batch size.
- Streaming decode from fragmented buffers.

## Validation plan

Unit tests begin from fixed original-LevelDB bytes and cover:

- Default header, exact Put/Delete encodings, empty/binary keys and values.
- Sequence and count accessors.
- Ordered reading with per-entry sequences.
- Copy, invariant-preserving move, clear, append, empty append, and self-append.
- Mutations whose key/value views alias the batch representation.
- Oversized sequence ranges and mutation failure atomicity.
- Truncated header, varints, keys, and values.
- Unknown tags, count mismatch, extra valid records, and trailing garbage.
- Original LevelDB's non-canonical terminal-varint behavior.
- Borrowed reader view lifetimes.

The uint32 count and length guards are inspected directly rather than exercised
with multi-gibibyte fixtures. A session-only differential helper compares
randomized encoded batches, append results, and parsing with unmodified
LevelDB. Normal tests remain self-contained.

## Consequences

- WAL and recovery share one exact batch representation.
- MemTable insertion can remain zero-copy while iterating one WAL record.
- Extended RocksDB operations require an explicit later format decision.
- The always-valid representation invariant simplifies later writer grouping
  and recovery.

## References

- [Google LevelDB write batch API](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/write_batch.h)
- [Google LevelDB write batch format](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/write_batch.cc)
- [Google LevelDB write batch tests](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/write_batch_test.cc)
- [RocksDB write batch API](https://github.com/facebook/rocksdb/blob/main/include/rocksdb/write_batch.h)
