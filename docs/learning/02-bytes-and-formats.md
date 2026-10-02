# 02. Bytes and Formats

[Learning path](README.md) | Next: [Memory and MVCC](03-memory-and-mvcc.md)

Prerequisite: [the engine map](01-the-engine-map.md).

## Bytes are not text

Keys and values can contain zero bytes and arbitrary binary data.
`ByteView` is a `std::span<const std::byte>`: a pointer and length, not an
owning string and not a null-terminated C string.

This matters when comparing keys, computing checksums, decoding a length,
and returning borrowed values. A zero byte does not end a key.
The default comparator orders keys lexicographically by bytes.

## Fixed-width integers and varints

Persistent data cannot rely on a machine's native endianness, structure
padding, or pointer size.

**Fixed-width little-endian encoding** stores a known number of bytes:

```text
fixed32(0x12345678) = 78 56 34 12
```

The least significant byte comes first.

**Varint encoding** stores seven value bits per byte. The high bit says
whether another byte follows:

```text
300 = 44 + 2 * 128
varint32(300) = ac 02
                 |  |
                 |  +-- value 2, stop
                 +----- value 44, continue
```

Small lengths usually need only one byte. Large lengths need more.
Varints are useful for compact lengths and offsets; fixed-width trailers
are useful when the decoder must find fields directly from a known position.

Checked `Consume*` helpers update the input view only on success.
This lets a caller propagate an error without accidentally continuing from
a partially consumed representation.

## Internal keys

A user key alone cannot distinguish its old and new values. The engine adds
an eight-byte trailer:

```text
internal key = user key || fixed64((sequence << 8) | kind)

kind 0 = Deletion
kind 1 = Value
sequence uses 56 bits
```

For user key `"a"`, sequence 42, and Value:

```text
61 | 01 2a 00 00 00 00 00 00
 a | trailer 0x2a01, little-endian
```

The **comparator**, not a raw comparison of the complete byte encoding,
defines internal-key ordering:

1. Compare user keys using the configured user comparator.
2. For equal user keys, compare trailers in descending numeric order.

Thus `"color"@42` appears before `"color"@41`.
Descending history makes a seek at a snapshot land on the newest visible
entry instead of scanning forward from the oldest one.

Custom comparators must keep the same identity and ordering semantics on
every reopen. Renaming a comparator does not migrate already sorted files,
and reusing its name with different semantics can invalidate their indexes.

## A write batch, byte by byte

A batch starts with a 12-byte header:

```text
starting sequence: fixed64
operation count:   fixed32
records:           ordered Put/Delete payloads
```

A Put record is:

```text
01 | varint32(key length) | key | varint32(value length) | value
```

A Delete record is:

```text
00 | varint32(key length) | key
```

For a batch assigned starting sequence 50:

```text
Put("a", "red")
Delete("b")

32 00 00 00 00 00 00 00 | 02 00 00 00
01 01 61 03 72 65 64
00 01 62
```

The operations receive sequences 50 and 51. The batch header stores only
the starting sequence, not one sequence per record.
The public application builds the operations; the engine assigns sequences
when it commits them.

An empty Put value is still a Value record. It is not a deletion.
Likewise, an empty key is valid.

## Framing and payload are separate

The same bytes have several surrounding representations:

| Representation | Job |
|---|---|
| Write-batch payload | Encode logical operations |
| WAL physical record | Frame and checksum fragments of that payload |
| Memtable entry | Pack an internal key and value for memory lookup |
| SSTable entry | Prefix-compress a sorted internal key and store its value |
| MANIFEST edit | Describe changes to files and metadata |

A WAL checksum can establish that a fragment's bytes match its stored
checksum. It does not establish that those bytes form a valid write batch.
Both layers have work to do.

## Hashes and checksums

`Hash32` supports cache sharding and Bloom filters. CRC32C detects accidental
corruption in WAL fragments and stored table blocks.
Neither is encryption or a cryptographic authenticity check.

LevelDB masks stored CRC values using a reversible rotation and addition.
Masking does not strengthen the checksum; it avoids problematic interactions
when checksummed data contains checksum values.
Hardware-dispatched CRC calculation must produce exactly the same bytes as
the portable calculation.

## Checked boundaries and trusted loops

`WriteBatchReader::Open` validates an external encoded batch completely:
header, lengths, tags, count, sequence range, and absence of trailing bytes.
Its `Next` can then return an optional entry rather than a format error for
every step.

The current owned-batch implementation stores its bytes in `std::string`.
Its mutators preserve a valid private representation, so the commit path
can use `OpenTrusted` without rescanning external data.
Early ADR-0016 described vector storage; ADR-0060 records this later change.

"Trusted" never means "bytes from disk are probably fine." It means a
specific caller has already established the required invariant and
preserves the storage and lifetime that make it true.
Table data entries, for example, are checked lazily as iterators reach them.

## Source tour

| File | Focus |
|---|---|
| [`bytes.h`](../../include/modern_leveldb/base/bytes.h) | Owning storage versus borrowed views |
| [`coding.cc`](../../src/base/coding.cc) | Fixed-width, varint, and failure-atomic consumption |
| [`internal_key.cc`](../../src/format/internal_key.cc) | Encoding, parsing, lookup keys, and ordering |
| [`write_batch.cc`](../../src/format/write_batch.cc) | Owned mutation and checked/trusted readers |
| [`internal_key_test.cc`](../../tests/unit/format/internal_key_test.cc) | Independent bytes and ordering examples |
| [`write_batch_test.cc`](../../tests/unit/format/write_batch_test.cc) | Malformed input, count mismatches, and aliasing |

Then read [ADR-0012](../adr/0012-internal-key-format.md),
[ADR-0016](../adr/0016-write-batch-format.md), and the owned-batch decisions
in [ADR-0060](../adr/0060-leveldb-write-path-parity.md).

## Self-check

1. Encode 128 as a varint.
2. Which sorts first: `(a, 7, Value)` or `(a, 9, Deletion)`?
3. Why is a successful encode/decode round trip insufficient to prove
   LevelDB format compatibility?

<details>
<summary>Answers</summary>

1. `80 01`.
2. Sequence 9's deletion sorts first. Sequence takes precedence over kind.
3. An encoder and decoder can share the same wrong convention.
   Independent golden bytes or a reference implementation are needed.

</details>
