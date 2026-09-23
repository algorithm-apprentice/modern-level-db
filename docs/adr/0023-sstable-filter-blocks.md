# ADR-0023: SSTable Filter Blocks

- Status: Accepted
- Date: 2026-09-24

## Context

An SSTable may contain one filter block that lets reads skip data blocks that
cannot contain a key. LevelDB stores one filter for every 2 KiB of data block
offsets and uses a Bloom filter policy. The table's metaindex block maps
`filter.<policy name>` to the filter block's handle.

This implements only the `implement-filter` DAG node: the Bloom filter policy,
the filter block builder, and the filter block reader. Writing the filter
block and its metaindex entry, reading it from a table file, and deciding
which keys enter the filter remain in the SSTable writer and reader nodes.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| SSTable writer | Start a filter range for each data block offset, add each key, and finish the filter block; name its metaindex entry after the policy |
| SSTable reader | Validate a filter block read from a table and ask whether a key may be in the data block at an offset |

LevelDB builds table filters over user keys: its `InternalFilterPolicy`
removes the internal-key trailer before building and probing filters. The
SSTable nodes pass user keys to these types directly, so no wrapper policy is
needed.

No current caller requires custom filter policies, full-table or partitioned
filters, or filters over key prefixes.

## Prior art

### Google LevelDB

Adopt the Bloom filter and filter block formats exactly.

A Bloom filter over `n` keys has `max(64, n * bits_per_key)` bits, rounded up
to whole bytes, followed by one byte holding the probe count
`k = clamp(floor(bits_per_key * 0.69), 1, 30)`. Each key hashes with LevelDB's
hash and seed `0xbc9f1d34`; probe `j` sets bit `(h + j * delta) mod bits`, in
32-bit arithmetic, where `delta` rotates `h` right by 17 bits. Bits are stored
least significant first: bit `p` is `1 << (p % 8)` of byte `p / 8`. The policy
name is `leveldb.BuiltinBloomFilter2`.

Matching follows LevelDB for every byte string: a filter shorter than two
bytes matches nothing, and a probe count above 30, which LevelDB reserves for
future encodings, matches every key.

A filter block stores its filters, then the fixed32 offset of each filter,
then the fixed32 offset of that array, then the base-2 logarithm of the range
size, which is always 11. Filter `i` covers data blocks that start in
`[i * 2048, (i + 1) * 2048)`. Ranges before the last one with keys have empty
filters, and no filter follows the last range with keys, so a table without
keys has the five-byte filter block `00 00 00 00 0b`.

Change:

- **Concrete Bloom policy.** LevelDB's filter types take a polymorphic
  `FilterPolicy`. The only current policy is Bloom, so the builder and reader
  keep their own copy of a `BloomFilterPolicy`, which is two integers. An
  interface waits for a public API that accepts custom policies.
- **Bounded filter blocks.** LevelDB narrows offsets silently if a filter block
  exceeds 4 GiB. Here, adding a key or starting a block that would make the
  finished block exceed `UINT32_MAX` bytes fails before allocating.
- **Validate filter blocks once.** LevelDB's reader treats a malformed filter
  block as matching every key. Creating a reader here returns `Corruption`
  unless the block has the layout LevelDB writes, so the SSTable reader can
  report the damage.
- **Integer probe count.** Compute `k` as `bits_per_key * 69 / 100` in 64-bit
  integers instead of floating point. For every nonnegative `bits_per_key` this
  equals LevelDB's result after clamping, because `bits_per_key * 0.69` is
  never within rounding error of an integer below 30.

### RocksDB

RocksDB replaced this block-based filter with full and partitioned filters,
cache-local Bloom filters, and Ribbon filters, and removed block-based filters
in version 7. These are rejected because they change the table format and no
current caller needs them.

## Decision

Add `src/table/bloom_filter.{h,cc}`:

```cpp
class BloomFilterPolicy {
 public:
  explicit BloomFilterPolicy(std::uint32_t bits_per_key) noexcept;
  std::string_view Name() const noexcept;
  std::uint64_t FilterSize(std::size_t key_count) const noexcept;
  void CreateFilter(std::span<const ByteView> keys, std::vector<std::byte>& output) const;
  bool KeyMayMatch(ByteView key, ByteView filter) const noexcept;
};
```

`CreateFilter` appends one filter of `FilterSize(keys.size())` bytes and
leaves existing bytes unchanged. Any `bits_per_key` is accepted and follows
LevelDB's formulas; the public API validates user options. Sizes and bit
positions use 64-bit arithmetic regardless of the width of `std::size_t`, and
`FilterSize` saturates instead of wrapping.

Add `src/table/filter_block.{h,cc}`:

```cpp
class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(BloomFilterPolicy policy) noexcept;
  Status StartBlock(std::uint64_t block_offset);
  Status AddKey(ByteView key);
  ByteView Finish();
};

class FilterBlockReader {
 public:
  static Result<FilterBlockReader> Create(std::vector<std::byte> contents,
                                          BloomFilterPolicy policy);
  bool KeyMayMatch(std::uint64_t block_offset, ByteView key) const noexcept;
};
```

### Builder

- `StartBlock` takes the offset of each data block, which must not decrease,
  and generates the filters of every earlier range.
- `AddKey` adds a key of the current data block.
- `StartBlock` and `AddKey` compute the size of the finished block before
  changing anything and return `InvalidArgument`, leaving the builder
  unchanged, if it would exceed `UINT32_MAX` bytes, because the block's
  offsets are 32-bit. A huge block offset or `bits_per_key` reaches these
  guards without large inputs, so tests cover them.
- `Finish` generates a filter for the last range only if it has keys, then
  appends the offset array, so it reports no errors. The returned view stays
  valid until the builder is destroyed, and `Finish` may be called only once.
- An exception, which only allocation failure raises, leaves the builder
  unusable, as for `BlockBuilder` in
  [ADR-0022](0022-sstable-block-format.md). Callers abandon the table being
  built in that case.
- The builder is neither copyable nor movable.

### Reader

`FilterBlockReader::Create` takes ownership of the contents and returns
`Corruption` unless:

- the block holds the array offset and the range size;
- the range size is 2 KiB;
- the offset array lies between the array offset and the trailing five bytes
  and holds whole fixed32 values;
- filter offsets start at zero, do not decrease, and do not exceed the array
  offset, and a block without filters has array offset zero.

The reader is movable but neither copyable nor assignable. `KeyMayMatch`
returns the policy's result for the filter covering the offset; an offset
beyond the last filter may match, because a missing filter must not hide keys.

## Explicitly deferred behavior

- Custom filter policies and a public `FilterPolicy` interface.
- Removing internal-key trailers, writing the metaindex entry, and reading the
  filter block from files, which belong to the SSTable nodes.
- Full-table, partitioned, and Ribbon filters.

## Validation plan

Unit tests cover:

- The probe count formula at its clamping boundaries and filter sizes.
- LevelDB's Bloom tests: an empty filter, a small filter, and false-positive
  rates below 2% across filter lengths.
- Exact golden bytes for a Bloom filter and for filter blocks produced by
  LevelDB.
- Matching of short filters and reserved probe counts.
- Filter ranges across 2 KiB boundaries, empty ranges, the empty builder, and
  offsets beyond the last filter.
- Both size guards, with the builder left unchanged.
- Every reader rejection.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

A session-only differential helper compares Bloom filters, match results over
random filters, and filter blocks with unmodified LevelDB in both directions,
reads the filter blocks of LevelDB tables, and checks that the reader never
reports a definite miss where LevelDB's reader would report a match.

## Consequences

- Filter blocks and Bloom filters are byte-compatible with LevelDB.
- Tables whose filter blocks are damaged fail validation instead of silently
  losing their filters.
- Supporting custom filter policies later requires introducing an interface
  over the Bloom policy.

## References

- [Google LevelDB Bloom filter policy](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/bloom.cc)
- [Google LevelDB filter blocks](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/filter_block.cc)
- [Google LevelDB table format](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/doc/table_format.md)
- [Google LevelDB internal filter policy](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/dbformat.cc)
- [RocksDB filters](https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter)
