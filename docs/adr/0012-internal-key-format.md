# ADR-0012: Internal Key Format

- Status: Accepted
- Date: 2026-09-22

## Context

An LSM tree stores multiple versions of a user key. Each stored entry needs a
sequence number for snapshot visibility and a kind distinguishing a value from
a deletion tombstone. Memtables, SSTables, manifests, iterators, and
compactions must agree on one stable byte representation and ordering.

This implements only the `implement-internal-key` DAG node. Lookup-key
prefixes, memtable entry encoding, write batches, filter adaptation, range
deletions, and extended value kinds remain separate future work.

## Current Modern LevelDB callers

| Future caller | Required internal-key behavior |
|---|---|
| Memtable and write batch | Encode a user key, sequence number, and value kind |
| Point lookup | Form a seek key at a visible sequence |
| SSTable blocks/indexes | Compare encoded internal keys and shorten index keys |
| Version metadata | Own, copy, serialize, and inspect smallest/largest keys |
| Iteration/compaction | Parse user key, sequence, and kind without allocation |

No current caller requires merge operands, single-delete, range tombstones,
column families, user timestamps, blobs, synthetic keys, or batch sequence
bits.

## Prior art and adopted decisions

### Google LevelDB

Adopt the format and ordering exactly:

```text
internal key = user key || fixed64 little-endian trailer
trailer      = (sequence << 8) | kind
```

- Sequence numbers occupy 56 bits and range from zero through `2^56 - 1`.
- `Deletion` is kind `0`; `Value` is kind `1`.
- User keys sort through the configured user comparator.
- Equal user keys sort by trailer in descending unsigned order, which means
  newer sequence numbers precede older versions and `Value` precedes
  `Deletion` at an equal sequence.
- A seek key uses kind `Value`, the largest supported LevelDB kind, so it sorts
  before all visible entries at lower sequence numbers.
- Index separators and successors use the maximum sequence number and seek
  kind after shortening the user key.

### RocksDB and Pebble

Both retain LevelDB's 8-byte trailer and 56-bit sequence layout, demonstrating
the durability of the format. They add many kinds and semantics for merge,
single-delete, range keys, timestamps, blobs, ingestion, and transactions.
Modern LevelDB deliberately rejects those extensions until a concrete feature
requires an incompatible format decision.

## Decision

### Persistent constants and types

```cpp
using SequenceNumber = std::uint64_t;

inline constexpr SequenceNumber MaxSequenceNumber =
    (std::uint64_t{1} << 56U) - 1U;
inline constexpr std::size_t InternalKeyTrailerSize = 8;

enum class ValueKind : std::uint8_t {
  Deletion = 0,
  Value = 1,
};

inline constexpr ValueKind SeekValueKind = ValueKind::Value;
```

The numeric enum values and trailer size are persistent format constants and
must not change.

### Borrowed parsed representation

```cpp
struct ParsedInternalKey {
  ByteView user_key;
  SequenceNumber sequence;
  ValueKind kind;
};

Result<ParsedInternalKey> ParseInternalKey(ByteView encoded);
```

`ParsedInternalKey::user_key` borrows from the encoded input. Parsing does not
allocate or modify input.

- Inputs shorter than eight bytes return `Corruption`.
- Trailer kind bytes other than `0` and `1` return `Corruption`.
- An empty user key followed by a valid trailer is valid.

### Immutable owning representation

```cpp
class InternalKey {
 public:
  static Result<InternalKey> Create(
      ByteView user_key, SequenceNumber sequence, ValueKind kind);
  static Result<InternalKey> Decode(ByteView encoded);

  ByteView encoded() const noexcept;
  ByteView user_key() const noexcept;
  SequenceNumber sequence() const noexcept;
  ValueKind kind() const noexcept;
};
```

`InternalKey` owns a `std::vector<std::byte>` and is copyable and movable as a
normal metadata value. It has no default invalid state and no mutating setter.
Returned byte views remain valid until the key is destroyed, moved from, or
assigned.

`Create` rejects sequence numbers above `MaxSequenceNumber` and invalid enum
values with `InvalidArgument`. `Decode` validates before copying and reports
malformed persistent input as `Corruption`. Allocation failures follow normal
C++ allocation behavior.

No general append API is added before a concrete caller needs direct encoding
into another buffer or arena.

### Internal comparator

```cpp
class InternalKeyComparator final : public Comparator {
 public:
  explicit InternalKeyComparator(const Comparator& user_comparator);

  int Compare(ByteView left, ByteView right) const noexcept override;
  std::string_view Name() const noexcept override;
  void FindShortestSeparator(
      std::vector<std::byte>& start, ByteView limit) const override;
  void FindShortSuccessor(std::vector<std::byte>& key) const override;
};
```

The comparator borrows a user comparator that must outlive it. For valid keys,
ordering exactly matches LevelDB.

The base comparator cannot return an error. To avoid undefined behavior on
corrupted inputs, malformed keys have a deterministic total order:

- Malformed keys sort before valid keys.
- Two malformed keys use bytewise comparison of their complete encodings.

Separator and successor operations leave malformed inputs unchanged. For valid
inputs, they shorten only the user-key portion. A replacement is accepted only
when it is physically shorter, logically greater than the original user key,
and still less than the limit where applicable. The replacement trailer is
`(MaxSequenceNumber, SeekValueKind)`.

The comparator name is `leveldb.InternalKeyComparator`.

## Explicitly deferred behavior

- `LookupKey` and memtable length-prefix encoding.
- Debug-string escaping and diagnostic formatting.
- Filter-policy adaptation from internal keys to user keys.
- Additional value kinds or format versioning.
- User timestamps, range-key sentinels, and batch sequence markers.
- Mutable or invalid owning internal-key states.

## Validation plan

Unit tests begin with independent LevelDB-compatible golden bytes and cover:

- Empty, binary, and ordinary user keys.
- Sequence boundaries around 8, 16, 32, and 56 bits.
- Exact trailer little-endian layout.
- Invalid sequence and kind rejection.
- Truncated and malformed persistent input.
- Borrowed parsed views and immutable owning-copy behavior.
- User-key ascending, sequence descending, and kind descending ordering.
- Strict deterministic ordering for malformed encodings.
- LevelDB separator and successor cases.

An isolated reference helper may compile unmodified LevelDB to generate
additional vectors, but normal tests contain fixed expectations and do not
download or build upstream code.

## Consequences

- Initial SSTables and manifests can remain compatible with the original
  LevelDB on-disk format.
- The small kind set makes parsing and compaction semantics explicit.
- New operation kinds require a later format ADR rather than silently
  consuming unused trailer values.
- Lookup-specific memory layout stays out of the format node until the
  memtable requires it.

## References

- [Google LevelDB internal-key format](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/dbformat.h)
- [Google LevelDB internal-key implementation](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/dbformat.cc)
- [RocksDB internal-key format](https://github.com/facebook/rocksdb/blob/main/db/dbformat.h)
- [Pebble internal keys](https://github.com/cockroachdb/pebble/blob/master/internal/base/internal.go)
