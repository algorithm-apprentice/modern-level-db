# ADR-0018: WAL Stream Reader and Writer

- Status: Accepted
- Date: 2026-09-23

## Context

The physical WAL format is already implemented as pure fragmentation and
decoding. The engine now needs streaming I/O that writes complete logical
records, reads 32 KiB blocks, reassembles fragmented records, exposes
corruption without hiding recovery policy, and preserves the filesystem
durability contract.

This implements only the `implement-wal-io` DAG node. Filename policy, WAL
creation/rotation, writer grouping, write-batch application, MANIFEST replay,
database recovery, log reuse decisions, and obsolete-file deletion remain
later nodes.

## Layer placement

WAL streaming depends on both the `platform` file interfaces and pure `format`
framing. It therefore cannot live in either lower layer:

- `platform` must not know LevelDB record semantics.
- `format` must remain free of filesystem I/O.

Add a `src/wal/` layer alongside `memory`, below table, metadata, and engine
orchestration.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Write path | Append one encoded write batch and know bytes reached the kernel |
| Synchronous write | Explicitly sync the WAL after append |
| Recovery | Iterate records, inspect corruption, and choose continue/stop policy |
| VersionSet | Reuse the same stream framing for MANIFEST records |
| Append/reopen | Safely append after an existing file without joining a truncated tail |
| Compatibility/fault tests | Inject short reads, I/O failures, corruption, and truncation |

No current caller requires async group sync, WAL recycling, compression,
log-number headers, direct I/O, preallocation, tailing a growing file,
concurrent writer calls, or reading from a nonzero initial offset.

## Prior art and adopted decisions

### Google LevelDB

Adopt:

- A non-copyable sequential writer with block-offset state.
- A block-buffered sequential reader.
- Exact logical fragmentation and reassembly.
- Mandatory checksum verification.
- Benign treatment of a truncated final header/payload and an incomplete final
  fragmented record.
- Silent compatibility with the historical empty `First` fragment at an exact
  seven-byte block boundary.
- Corruption isolation that never joins fragments across a bad physical record.

Change:

- Own open file handles through `std::unique_ptr`.
- Return filesystem I/O failures as typed `Error` values.
- Return corruption as typed stream events instead of an optional reporter
  callback, allowing each caller to stop or continue explicitly.
- Flush once after the complete logical record rather than after every physical
  fragment. This preserves the WAL-before-MemTable boundary while avoiding
  redundant flush calls.

Reject:

- Initial-offset seeking and resynchronization. Every LevelDB production
  reader (DB recovery, MANIFEST recovery, repair, and dump) starts at offset
  zero; only LevelDB's log tests use a nonzero offset.

### RocksDB

RocksDB retains the same core reader/writer but adds recycled formats,
compression, WAL-number verification, recovery modes, mutable EOF for tailing,
multi-read optimizations, and async/manual flush paths. These are rejected
because current callers require only immutable legacy logs.

### Pebble

Pebble adds asynchronous sync queues, concurrent commit coordination, failover,
metrics, and richer writer lifecycle. Modern LevelDB defers those mechanisms
to the future write-path node; this node remains single-threaded and
synchronous.

## Decision

### Writer ownership and API

```cpp
class WalWriter final {
 public:
  explicit WalWriter(
      std::unique_ptr<WritableFile> file,
      std::uint64_t initial_file_size = 0);

  Status AddRecord(ByteView logical_record);
  Status Sync();
  Status Close();
};
```

The constructor takes sole ownership. A null handle creates a poisoned writer
whose operations return `InvalidArgument`; it never dereferences null.
WalWriter is non-copyable and non-movable. `initial_file_size` must match the
actual file length supplied by the caller.

If a non-empty existing file ends inside a 32 KiB block, the first new record
pads the entire remaining block with zeros and starts in the next block. It
does not resume directly at raw EOF. This prevents a crash-truncated header or
payload from consuming bytes from the newly appended record on the next read.
An existing size already aligned to a block needs no padding.

`AddRecord`:

1. Uses `WalFragmenter` to produce the complete physical sequence.
2. Appends zero padding, header, and payload bytes in exact order.
3. Calls `WritableFile::Flush` once after every logical record, including an
   empty record.
4. Returns success only after all bytes have left the writable file's
   user-space buffer.

This does not make the record crash-durable. The future write path calls
`Sync()` when requested and only then exposes a synchronous write as durable.
`Sync()` delegates to `WritableFile::Sync`; `Close()` delegates to the terminal
file close and does not imply sync.

Any append, flush, or sync failure poisons the writer. Because the fragmenter
has already advanced and the file may contain a partial logical record,
subsequent `AddRecord` and `Sync` calls return the first error without further
I/O. `Close` still attempts to release the file and returns the first observed
error. Calling any operation after `Close`, including `Close` again, returns
`InvalidArgument`.

Allocation failure while fragmenting occurs before I/O and does not poison the
writer. The owned file's destructor remains the best-effort fallback when
explicit `Close` is omitted.

### Reader events

```cpp
struct WalLogicalRecord {
  ByteView data;
  std::uint64_t offset;
};

struct WalCorruption {
  Error error;
  std::uint64_t dropped_bytes;
  std::uint64_t offset;
};

using WalReadEvent = std::variant<WalLogicalRecord, WalCorruption>;
using WalReadResult = Result<std::optional<WalReadEvent>>;

class WalReader final {
 public:
  explicit WalReader(std::unique_ptr<SequentialFile> file);

  WalReadResult ReadNext();
};
```

The constructor takes sole ownership. A null handle creates a terminal reader
whose calls return `InvalidArgument`; it never dereferences null. WalReader is
non-copyable and non-movable. Reading always starts at file offset zero.

`ReadNext()` returns:

- `WalLogicalRecord` for one complete logical record.
- `WalCorruption` for one recoverable corruption event. The next call resumes
  from the reader's already-updated stream position.
- `nullopt` at EOF. EOF is idempotent.
- `unexpected(Error)` for a file `Read` failure or an oversized read count. I/O
  failure is terminal and later calls return the same error.

The record view borrows reader-owned block or scratch storage and remains valid
until the next `ReadNext()` call or reader destruction. Corruption events own
their `Error`.

### Block filling and short reads

The reader owns one 32 KiB block buffer. `SequentialFile::Read` is allowed to
return a short successful read that is not necessarily EOF, so the reader
repeats reads into the remaining block space until:

- The block is full.
- A read returns zero, marking EOF.
- A read fails.

Returned byte counts larger than the supplied output view are treated as `Io`
interface violations. Byte offsets are bounded by the file length and need no
overflow handling.

Fewer than seven remaining bytes in a non-final block are zero-padding/trailer
and are discarded silently. At final EOF, a short header is also ignored as a
crash-truncated tail.

### Physical decode and corruption recovery

Each decode call receives only unread bytes from one block.

- A valid fragment advances by its encoded size.
- A zero marker discards the remainder of the current block without itself
  reporting physical corruption. It always clears partial logical reassembly;
  a non-empty abandoned partial payload emits one logical corruption event
  before any later record is read.
- `SkipPhysicalRecord` advances by the trusted encoded size and emits one
  corruption event.
- `DropBlock` discards all unread bytes in the current block and emits one
  corruption event.
- A truncated payload in the final partial block is benign EOF.
- A complete record with a bad checksum remains corruption even at EOF.

`dropped_bytes` is an approximate count of bytes rendered unusable. Depending
on the failure, it counts a fragment payload, a trusted physical record, or the
unread block, plus any already assembled logical payload. `offset` identifies
the first affected logical-record start when known, otherwise the physical
record start.

### Logical reassembly

The reader maintains at most one partial logical record.

- `Full` returns immediately.
- `First` starts scratch accumulation.
- `Middle` extends an active record.
- `Last` completes and returns the scratch-backed record.
- `Middle` or `Last` without `First` emits corruption.
- A non-empty partial record interrupted by `Full` emits corruption first and
  returns that `Full` record on the next call without additional file I/O.
- A non-empty partial record interrupted by another `First` emits corruption
  while retaining the new `First` as the beginning of the next record.
- An empty `First` followed by `Full` or `First` is silently replaced to
  preserve LevelDB's historical exact-header-boundary compatibility.
- Physical corruption clears partial state so fragments from different logical
  records can never be joined.
- EOF while a record is incomplete silently discards the tail.

The `offset` in every `WalLogicalRecord` is the physical offset of its `Full`
or `First` fragment.

### Reader structure

The reader has two private stages, mirroring LevelDB's
`ReadPhysicalRecord`/`ReadRecord` division:

- A physical block scanner owns block refill, trailers, crash-truncated tails,
  decoder recovery, and zero markers. It returns one physical fragment, one
  physical corruption, or EOF.
- A logical assembler owns fragment state and conversion of abandoned partial
  records into corruption events.

When a `Full` or `First` fragment interrupts a non-empty partial record, the
assembler emits the corruption event and leaves that fragment unconsumed in the
current block. The next call decodes it again from memory, so no separate
pending-record state is required.

### Threading

WalReader and WalWriter require external synchronization and are intended for
one calling thread. Their owned file handles are never accessed concurrently
through another owner.

## Explicitly deferred behavior

- WAL filename creation, file-number allocation, and directory sync.
- WAL rotation, reuse, recycling, preallocation, or truncation.
- Write batching, sequence assignment, and MemTable insertion.
- Recovery mode policy such as paranoid stop versus best-effort continuation.
- Applying `WriteBatchReader` records.
- MANIFEST/version replay semantics.
- Tailing files that may grow after EOF.
- Initial-offset seeking and resynchronization.
- Concurrent writer calls, asynchronous sync, or group-commit queues.
- Compressed, recyclable, log-number, and sync-offset record types.
- Metrics, logging callbacks, or general event-subscriber frameworks.

## Validation plan

Unit tests use in-memory file doubles and cover:

- Empty, small, empty-payload, multi-block, and many sequential records.
- Exact writer bytes, append/reopen block offsets, padding, and the historical
  exact-seven-byte boundary.
- One flush per logical record, explicit sync, close, and terminal first-error
  retention.
- Arbitrarily short successful reads that still fill complete blocks.
- Mandatory checksum verification.
- Unknown types, checksum mismatch, bad lengths, zero markers, unexpected
  fragment types, interrupted fragmented records, and corruption isolation.
- Benign truncated final headers/payloads and missing final fragments.
- Record-view lifetime until the next read.
- Read I/O errors, oversized read counts, and terminal reader failure.
- Every coverable line and branch of the WAL stream implementation, measured
  with a local coverage build.

POSIX integration writes, syncs, closes, reopens, and reads an actual WAL file.
A session-only differential helper compares writer bytes, logical records,
offsets, and representative corruption behavior with unmodified LevelDB.

## Consequences

- Write-path code receives an explicit kernel-flush boundary and separate
  durability sync.
- Recovery policy remains outside the stream parser.
- Corruption can be surfaced without losing a valid record immediately after
  an abandoned fragmented record.
- Reader-owned borrowed views avoid a copy for full physical records.
- The new `wal` layer preserves the platform/format dependency direction.

## References

- [Google LevelDB log writer](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/log_writer.cc)
- [Google LevelDB log reader](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/log_reader.cc)
- [Google LevelDB log tests](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/log_test.cc)
- [RocksDB log writer](https://github.com/facebook/rocksdb/blob/main/db/log_writer.h)
- [RocksDB log reader](https://github.com/facebook/rocksdb/blob/main/db/log_reader.h)
- [Pebble record writer](https://github.com/cockroachdb/pebble/blob/master/record/log_writer.go)
