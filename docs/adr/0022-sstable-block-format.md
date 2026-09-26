# ADR-0022: SSTable Block Format

- Status: Accepted

The original uncompressed-only scope is extended by
[ADR-0039](0039-sstable-block-compression.md). Trailers now record the selected
type, and verified Snappy and Zstd blocks are decoded instead of returning
`NotSupported`.
- Date: 2026-09-23

## Context

An SSTable is a sequence of blocks followed by a fixed-size footer. Data,
index, and metaindex blocks share one sorted key/value block encoding. Every
stored block carries a trailer with its compression type and checksum, and
block handles locate blocks by offset and size.

This implements only the `implement-block-format` DAG node: block handles, the
footer, block trailers, the block builder, and the block reader. Reading blocks
from files, table construction, filter blocks, and caching remain later nodes.
The code forms the `table` layer and performs no I/O.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| SSTable writer | Build data, index, and metaindex blocks with a restart interval; estimate the size of the block being built; encode block trailers, block handles, and the footer |
| SSTable reader | Decode the footer and index values; verify and strip the trailer of a block read by handle; seek and iterate in both directions within index and data blocks |
| Table and block caches | Own immutable blocks that iterators share |

No current caller requires compression, iterator error states, seeks that
reuse the current position, hash indexes, delta-encoded index values, or
alternative checksum types.

## Prior art

### Google LevelDB

Adopt the persistent format exactly.

A block stores entries followed by a restart array:

```text
entry:    shared varint32 | non_shared varint32 | value_length varint32
          | key_delta[non_shared] | value[value_length]
trailer:  restart_offset fixed32 ... | restart_count fixed32
```

Each key is stored as the length of the prefix it shares with the previous key
and the remaining bytes. Every `restart_interval` entries, an entry stores its
full key with `shared = 0`, and its offset is recorded in the restart array.
Readers binary search the restart points and then scan forward.

A block handle is a varint64 offset followed by a varint64 size, at most 20
bytes. A stored block is followed by a five-byte trailer: a compression type
byte and the masked CRC32C of the contents and the type byte. The 48-byte
footer holds the metaindex and index handles, zero padding to 40 bytes, and
the fixed64 magic number `0xdb4775248b80fb57`.

Change:

- **No compression in this node.** Write every block uncompressed. Decoding a
  block whose type is Snappy (1) or Zstd (2) returns `NotSupported`; any other
  type except none (0) is `Corruption`. Compression needs third-party
  libraries, and the MVP does not need it, so the later
  `implement-compression` node adds it before the compatibility harness, which
  reads LevelDB databases written with LevelDB's default Snappy compression.
  Until then, the SSTable compatibility of
  [ADR-0002](0002-leveldb-format-compatibility.md) covers uncompressed tables:
  LevelDB reads every table Modern LevelDB writes, and Modern LevelDB reads
  LevelDB tables written without compression.
- **Always verify checksums.** LevelDB verifies block checksums only when
  `ReadOptions::verify_checksums` or paranoid checks are enabled. CRC32C is
  cheap, and an unchecked block can return wrong data silently.
- **Validate blocks once.** Creating a block checks its whole structure and
  key order, so iterators cannot fail and have no error status. LevelDB checks
  entries lazily, reports corruption through the iterator status, and never
  checks key order.
- **Reject blocks LevelDB never writes.** A block must contain at least one
  restart point; the first restart point must be offset zero; and the footer
  padding must be zero.
- **Simple seek.** Every seek binary searches the restart points. LevelDB's
  optimization that starts from the current position is deferred until a
  benchmark justifies it.

### RocksDB

RocksDB's block-based table keeps this layout for its legacy format version
and adds data block hash indexes, delta-encoded index values, format versions,
alternative checksum types, many compression types, and a different footer.
These are rejected because no current caller needs them and each one changes
the persistent format.

## Decision

Add `src/table/block_format.{h,cc}`:

```cpp
inline constexpr std::size_t BlockHandleMaxEncodedSize = 20;
inline constexpr std::size_t BlockTrailerSize = 5;
inline constexpr std::size_t FooterSize = 48;
inline constexpr std::uint64_t TableMagicNumber = 0xdb4775248b80fb57;

struct BlockHandle {
  std::uint64_t offset;
  std::uint64_t size;
};

void AppendBlockHandle(std::vector<std::byte>& output, BlockHandle handle);
Result<BlockHandle> ConsumeBlockHandle(ByteView& input);

struct Footer {
  BlockHandle metaindex;
  BlockHandle index;
};

std::array<std::byte, FooterSize> EncodeFooter(const Footer& footer);
Result<Footer> DecodeFooter(std::span<const std::byte, FooterSize> encoded);

std::array<std::byte, BlockTrailerSize> EncodeBlockTrailer(ByteView contents);
Result<std::vector<std::byte>> DecodeStoredBlock(std::vector<std::byte> stored);
```

Add `src/table/block_builder.{h,cc}`:

```cpp
class BlockBuilder {
 public:
  explicit BlockBuilder(std::uint32_t restart_interval) noexcept;
  Status Add(ByteView key, ByteView value);
  ByteView Finish();
  void Reset() noexcept;
  std::size_t CurrentSizeEstimate() const noexcept;
  bool empty() const noexcept;
};
```

Add `src/table/block.{h,cc}`:

```cpp
class Block {
 public:
  static Result<Block> Create(std::vector<std::byte> contents,
                              const Comparator& comparator);

  class Iterator {
   public:
    explicit Iterator(const Block& block) noexcept;
    bool valid() const noexcept;
    ByteView key() const noexcept;
    ByteView value() const noexcept;
    void SeekToFirst();
    void SeekToLast();
    void Seek(ByteView target);
    void Next();
    void Prev();
  };
};
```

### Handles, footer, and trailers

- `ConsumeBlockHandle` consumes exactly one handle and leaves any following
  bytes, which LevelDB permits in index values, for the caller.
- `DecodeFooter` returns `Corruption` for a wrong magic number, a malformed
  handle, or nonzero padding.
- `EncodeBlockTrailer` records compression type none.
- `DecodeStoredBlock` takes the contents followed by the trailer, verifies the
  checksum, and returns the contents in the same buffer. It returns
  `Corruption` for input shorter than a trailer, a checksum mismatch, or an
  unknown type, and `NotSupported` for Snappy and Zstd blocks. The checksum
  covers the type byte, so a type is examined only after the checksum passes.

### Builder

- The restart interval must be at least one.
- Keys must be added in strictly increasing order under the comparator that
  will read the block. The builder does not compare keys; the SSTable writer
  validates the order of all keys in a table.
- `Add` returns `InvalidArgument` and changes nothing if the entry would make
  the finished block larger than `UINT32_MAX` bytes, because restart offsets
  and entry lengths are 32-bit. Only inputs larger than 4 GiB reach this
  guard, so its branch is excluded from coverage under
  [ADR-0019](0019-test-coverage-policy.md).
- `Finish` appends the restart array and returns the block, which stays valid
  until `Reset`. Adding after `Finish` requires `Reset`.
- An empty builder finishes to an eight-byte block with one restart point.
- The builder is neither copyable nor movable, so the view `Finish` returns
  cannot outlive its buffer.
- An exception, which only allocation failure raises, leaves the builder
  unusable until `Reset`. Callers abandon the table being built in that case.

### Reader

`Block::Create` takes ownership of the contents and returns `Corruption`
unless:

- the contents hold a restart count, which is at least one and fits before
  the count;
- the first restart point is offset zero;
- in a block without entries, zero is the only restart point; otherwise every
  restart point is the offset of an entry with `shared = 0`, and the restart
  points strictly increase;
- every entry lies within the entry region, the last entry ends exactly at
  the restart array, and each entry shares no more bytes than the previous key
  holds;
- keys strictly increase under the comparator.

LevelDB writes every data, index, and metaindex block this way, including the
empty index and metaindex blocks of an empty table. The block keeps the
comparator, which must outlive it, and its iterators use it. Offsets within a
block are `std::size_t` and only restart values are 32-bit, so no offset is
truncated even in a block larger than 4 GiB.

`Block` is move-constructible but neither copyable nor assignable. An
iterator copies the validated layout of the block, which views the contents
buffer; moving the block keeps that buffer and its iterators valid, and
destroying the block invalidates them. A new iterator is not positioned. `key`
and `value` require a valid position and remain valid until the iterator
moves. `Seek` positions at the first key not less than the target. Moving past
either end leaves the iterator invalid. If an iterator operation throws, the
iterator remains consistent at an unspecified position.

## Explicitly deferred behavior

- Snappy and Zstd compression, which belong to `implement-compression`.
- Reading blocks from files and checking handles against file sizes.
- Filter blocks and the meaning of index and metaindex entries.
- Seek optimizations, hash indexes, delta-encoded index values, alternative
  checksum types, and RocksDB format versions.

## Validation plan

Unit tests cover:

- Exact golden bytes for block handles, the footer, a trailer, and small
  blocks with several restart intervals.
- Handle and footer round trips at the varint boundaries and maximum values.
- Every decoding rejection for handles, footers, trailers, and blocks.
- Iteration and seeks in both directions against an ordered map model over
  random keys, restart intervals, and comparators, including empty blocks,
  empty keys and values, and multi-byte varint lengths.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

A session-only differential helper builds blocks with unmodified LevelDB and
Modern LevelDB from the same inputs and compares their bytes, reads each
implementation's blocks with the other, compares footers and trailers, and
reads uncompressed LevelDB tables, including an empty table, with the new
footer, trailer, and block code.

## Consequences

- Written blocks and footers are byte-compatible with LevelDB.
- Until `implement-compression`, Modern LevelDB cannot read LevelDB tables
  that use compression.
- Block iteration has no error path; every structural or ordering error
  surfaces when the block is loaded.

## References

- [Google LevelDB table format](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/doc/table_format.md)
- [Google LevelDB block handles and footer](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/format.cc)
- [Google LevelDB block builder](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/block_builder.cc)
- [Google LevelDB block reader](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/block.cc)
- [RocksDB block-based table format](https://github.com/facebook/rocksdb/wiki/Rocksdb-BlockBasedTable-Format)
