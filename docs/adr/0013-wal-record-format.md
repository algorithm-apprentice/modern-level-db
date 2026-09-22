# ADR-0013: WAL Record Format

- Status: Accepted
- Date: 2026-09-22

## Context

LevelDB stores write batches in a write-ahead log and stores MANIFEST edits in
the same record framing. Logical records may be larger than an I/O block, so
the format fragments them into independently checksummed physical records.

This implements only the `implement-wal-format` DAG node. Filesystem I/O,
buffer refill, initial-offset seeking, corruption reporting, logical-record
reassembly, and recovery policy remain in `implement-wal-io`.

## Current Modern LevelDB callers

| Future caller | Required format behavior |
|---|---|
| WAL writer | Fragment one logical write batch from the current file offset |
| MANIFEST writer | Use the same framing for version edits |
| WAL/MANIFEST reader | Parse and validate one physical fragment from buffered bytes |
| Fault and compatibility tests | Construct exact headers and mutate checksum, length, and type |

No current caller requires WAL recycling, compressed WAL records, log-number
headers, sync-offset records, async streaming, or direct-I/O alignment.

## Prior art and adopted decisions

### Google LevelDB

Adopt the legacy format exactly:

- The byte stream is divided into 32 KiB blocks.
- Physical records never cross block boundaries.
- Each physical record has a 7-byte header:

```text
+-------------------+----------------+----------+
| masked CRC32C 4 B | payload len 2 B| type 1 B |
+-------------------+----------------+----------+
```

- Checksum and payload length are little-endian.
- CRC32C covers the type byte followed by payload bytes, then uses LevelDB's
  mask transform before storage.
- Logical records use `Full`, `First`, `Middle`, and `Last` fragments.
- Type zero with zero length represents zeroed/preallocated space and is not a
  logical fragment.
- If fewer than seven bytes remain in a block, the writer emits zero padding
  and starts the next block.
- For byte-for-byte writer compatibility, if exactly seven bytes remain and a
  non-empty logical record follows, the writer emits LevelDB's historical
  zero-length `First` fragment before continuing in the next block.
- Empty logical records emit one zero-length `Full` fragment.

### RocksDB and Pebble

Both retain this legacy framing and add recyclable, compressed, log-number, or
sync-offset formats. Modern LevelDB rejects those extensions because no
current caller requires WAL recycling or cloud-storage corruption diagnosis.

## Decision

### Persistent constants

```cpp
inline constexpr std::size_t WalBlockSize = 32U * 1'024U;
inline constexpr std::size_t WalHeaderSize = 7;

enum class WalRecordType : std::uint8_t {
  Zero = 0,
  Full = 1,
  First = 2,
  Middle = 3,
  Last = 4,
};
```

These numeric values and sizes are persistent format constants.

### Fragmenter

```cpp
struct WalFragment {
  std::size_t padding_before;
  std::array<std::byte, WalHeaderSize> header;
  ByteView payload;
};

class WalFragmenter {
 public:
  explicit WalFragmenter(std::uint64_t initial_file_size = 0) noexcept;

  std::vector<WalFragment> Fragment(ByteView logical_record);
  std::size_t block_offset() const noexcept;
};
```

`WalFragmenter` is non-copyable and non-movable state owned by one future WAL
writer. `initial_file_size` supports append/reopen by setting the current block
offset to `initial_file_size % WalBlockSize`.

The returned fragments own their headers but borrow payload spans from
`logical_record`. The caller must consume them before the logical-record
storage changes or is destroyed. `padding_before` is in `[0, 6]` and tells the
I/O layer how many zero bytes to append before the header.

Fragmentation does not copy payload bytes. Allocation failure while building
the fragment vector follows standard C++ exception behavior and provides the
strong guarantee: `block_offset()` remains unchanged when `Fragment` throws.

A successful `Fragment` call atomically advances `block_offset()` through the
complete returned sequence. The future writer must emit all returned padding,
headers, and payloads in order. If I/O stops after a partial sequence, it must
discard the fragmenter or reconstruct one from the actual file size before
retrying.

### Physical fragment decoder

```cpp
struct DecodedWalFragment {
  WalRecordType type;
  ByteView payload;
  std::size_t encoded_size;
};

enum class WalDecodeKind {
  Fragment,
  EndOfBlock,
};

struct WalDecodeOutcome {
  WalDecodeKind kind;
  DecodedWalFragment fragment;
};

enum class WalRecoveryAction {
  SkipPhysicalRecord,
  DropBlock,
};

enum class WalDecodeFailure {
  TruncatedHeader,
  TruncatedPayload,
  PayloadTooLarge,
  ChecksumMismatch,
  UnknownType,
};

struct WalDecodeError {
  Error error;
  WalDecodeFailure failure;
  WalRecoveryAction recovery;
  std::size_t encoded_size;
};

std::expected<WalDecodeOutcome, WalDecodeError> DecodeWalFragment(
    ByteView encoded, bool verify_checksum = true);
```

The decoder consumes no input and does not allocate on success. Error
construction may allocate diagnostic text. For a fragment, `payload` borrows
from `encoded`; `encoded_size` is `WalHeaderSize + payload.size()` so a future
buffered reader can advance. `encoded_size` is zero for `DropBlock` failures
and trusted only for `SkipPhysicalRecord`.

The decoder:

- Returns `EndOfBlock` for `Zero` with zero length before checksum validation.
  This is a control outcome, not a logical fragment; the caller discards every
  remaining byte in the current block without reporting corruption.
- Returns a `DropBlock` error for a truncated header/payload, an impossible
  payload length, or a checksum mismatch. The future reader must not scan
  inside the remaining bytes because a corrupted length could point into
  arbitrary payload.
- Returns `SkipPhysicalRecord` for an unknown type after a trusted length and
  checksum. `encoded_size` identifies the complete physical record to skip.
- Treats `Zero` with nonzero length as an unknown type rather than an
  end-of-block marker.
- Verifies the unmasked stored checksum against CRC32C(type || payload) when
  `verify_checksum` is true.
- Ignores a mismatched stored checksum when verification is disabled.
- Applies validation in this order: header/length bounds, zero marker,
  checksum, then type validation. Therefore an unknown type with a bad
  checksum is `ChecksumMismatch`/`DropBlock`, not `UnknownType`/skip.
- Does not decide whether truncation at the physical end of a file is benign;
  that requires EOF and recovery context in `implement-wal-io`.
- Does not enforce a block offset. The buffered I/O layer supplies only bytes
  from one block and handles short block trailers.

## Header encoding

For each fragment:

1. Select type from the fragment's position in the logical record.
2. Compute `Crc32c(type_byte)` and extend it with the payload.
3. Mask the checksum.
4. Store checksum as fixed32 little-endian.
5. Store payload length as unsigned fixed16 little-endian.
6. Store the one-byte record type.

The maximum payload in a physical record is `32768 - 7 = 32761`, so it always
fits the 16-bit length field.

## Explicitly deferred behavior

- Reading from or writing to `SequentialFile`/`WritableFile`.
- Flush or sync decisions.
- Logical-record reassembly and dropped-byte reporting.
- Initial-offset resynchronization.
- Recovery treatment of truncated final records.
- Recyclable, compressed, or sync-offset record types.
- WAL file reuse and preallocation.

## Validation plan

Unit tests start from fixed bytes generated by unmodified Google LevelDB and
cover:

- Persistent constants and each record type.
- Empty and small full records.
- Exact checksum, length, type, and payload bytes.
- Fragmentation across two and multiple blocks.
- The six-byte trailer-padding boundary and exact-seven-byte historical quirk.
- Initial append offsets.
- Borrowed payload spans and fragmenter block-offset updates.
- Physical decode with checksum enabled and disabled.
- Zero-marker end-of-block behavior, valid-checksum unknown types that skip one
  record, invalid-checksum unknown types that drop the block, oversized
  lengths, truncated headers/payloads, and checksum corruption.

An isolated differential helper may compare generated byte streams against
unmodified LevelDB. Normal tests remain self-contained.

## Consequences

- WAL and MANIFEST writers can share one pure framing implementation.
- The format layer remains independent of filesystem and recovery policy.
- The historical exact-header-boundary behavior is intentionally preserved
  for byte-for-byte compatibility.
- Future WAL format extensions require a separate ADR and cannot silently
  consume additional record-type values.

## References

- [Google LevelDB log format constants](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/log_format.h)
- [Google LevelDB log writer](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/log_writer.cc)
- [Google LevelDB log reader](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/log_reader.cc)
- [RocksDB log format](https://github.com/facebook/rocksdb/blob/main/db/log_format.h)
- [Pebble record format](https://github.com/cockroachdb/pebble/blob/master/record/record.go)
