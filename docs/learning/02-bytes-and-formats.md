# 02. Bytes and Formats

[Learning path](README.md) | Next: [Memory and MVCC](03-memory-and-mvcc.md)

Prerequisite: [the engine map](01-the-engine-map.md).

## Lesson map

| Question | Answer |
|---|---|
| What problem is this lesson about? | Giving bytes a stable portable representation that can be validated at clear boundaries |
| Who consumes these formats? | WAL, table, metadata, MemTable, and lookup code |
| What interface matters first? | `ByteView`, coding helpers, InternalKey, EncodedWriteBatch, and WriteBatchReader |
| What implementation will we open? | Fixed-width encoding, varints, trailers, checked decoders, and trusted loops |
| What should you be able to do afterward? | Decode a batch and internal key on paper and identify where external validation ends |

## Bytes are not text

Keys and values can contain zero bytes and arbitrary binary data.
`ByteView` is a `std::span<const std::byte>`: a pointer and length, not an
owning string and not a null-terminated C string.

A view object and a C++ reference are different. Copying a `ByteView` copies
its pointer/length descriptor, not the referenced bytes. A `const ByteView&`
instead borrows the descriptor itself. Native LevelDB's `Slice` and
`const Slice&` have the same distinction. Read-path interfaces use references
at selected forwarding boundaries; stored/returned views and independent
parser cursors remain values. Neither form extends backing-storage lifetime.

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

For an unsigned w-bit value x, byte i contains
`floor(x / 2^(8*i)) mod 256`. Decoding sums those bytes multiplied by
`2^(8*i)`. This expansion proves the round trip for values in the model;
independent golden bytes are still needed to establish that the model
matches the persistent format.

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

### Derive the varint length and decoder invariant

Canonical base-128 emission repeatedly takes the low seven bits, sets their
continuation bit while more payload remains, and divides by 128. Thus:

```text
canonical bytes(x) = max(1, ceil(bit_length(x) / 7))
maximum bytes for a w-bit unsigned result = ceil(w / 7)
```

Zero still needs one terminal byte. These examples are canonical emissions:

<!-- canonical-varint-examples -->

| Value | Hex bytes | Byte count |
|---|---|---|
| 0 | `00` | 1 |
| 127 | `7f` | 1 |
| 128 | `80 01` | 2 |
| 300 | `ac 02` | 2 |
| 16383 | `ff 7f` | 2 |
| 16384 | `80 80 01` | 3 |
| 4294967295 | `ff ff ff ff 0f` | 5 |

After consuming i payload groups, the accumulator contains the unsigned
result-width truncation of
`sum(payload[j] * 2^(7*j), j=0..i-1)`. The next read must remain within the
input and the maximum group count. A terminal group completes the result;
a missing group is truncation, and continuation beyond the maximum is
overflow.

The compatible decoder accepts nonminimal encodings such as `80 00` for
zero, and truncates excess terminal payload bits to the result width.
Do not confuse the canonical encoder's range with a stricter decoder policy:
nonminimal representations can map to the same value. Continued input past
the maximum group count remains an error.

Checked `Consume*` helpers update the input view only on success.
This lets a caller propagate an error without accidentally continuing from
a partially consumed representation.

This requires a second invariant besides the accumulator: the caller's
cursor is unchanged until the complete operation succeeds. Decode through
a local cursor, validate the field or payload, then commit its advancement.
For `ac 02 55`, consuming a varint yields 300 and leaves `55`; for `80`,
failure leaves the entire original input. A length-prefixed field must
commit only after its declared payload also fits.

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

The byte layout does not itself implement that order. For one user key,
ordinary ascending comparison of the trailers for sequences 1 and 2 would
put 1 first; internal-key ordering requires 2 first. Search must use the same
numeric-trailer comparator as the writer's sorted representation.

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

In this example, the header occupies offsets 0 through 11. The Put tag is
at 12, its key length at 13, key at 14, value length at 15, and value at
16 through 18. The Delete tag, key length, and key occupy 19, 20, and 21.
The count describes logical operations, not the number of fields or bytes.

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
header, lengths, tags, count, and absence of trailing bytes. Like pinned
LevelDB, the batch codec does not impose an additional sequence-range policy.
Its `Next` can then return an optional entry rather than a format error for every
step.

The current owned-batch implementation stores its bytes in `std::string`.
Its mutators preserve the structural private representation, while the commit
boundary assigns the sequence interval. The commit path can therefore use
`OpenTrusted` without rescanning external data.
Early [ADR-0016](../adr/0016-write-batch-format.md) described vector storage;
[ADR-0060](../adr/0060-leveldb-write-path-parity.md) records this later
change.

"Trusted" never means "bytes from disk are probably fine." It means a
specific caller has already established the required invariant and
preserves the storage and lifetime that make it true.
Table data entries, for example, are checked lazily as iterators reach them.

For an index handle, `Table::Open` validates both complete uint64 varints and
their file bounds before publishing the immutable index. Get and iterator
loading can then share trusted decoding of those same stable bytes without
repeating public error construction. This proof does not apply to unchecked
data entries or externally modified files.

Fixed-word helpers follow the same separation: a proven complete word can
use an alignment-safe copy on a little-endian host, with byte assembly on
other endianness. The persistent representation remains little-endian;
checked consumers still reject short inputs without advancing their cursor.

## Costs depend on what the operation promises

For a fixed result width, a varint examines at most five or ten bytes.
For a width parameter allowed to grow, the bound is proportional to its
number of seven-bit groups. Copying an L-byte key/value costs at least
proportional byte work; returning a borrowed view does not copy L bytes.

Structural validation can check a declared payload extent and skip it
without comparing every payload byte. Checksums and copying do examine
bytes. Do not describe these operations as equal work merely because they
all return a view over the same data.

## Exercises: derive a representation and its parser

1. Encode 128 as a varint.
2. Which sorts first: `(a, 7, Value)` or `(a, 9, Deletion)`?
3. Why is a successful encode/decode round trip insufficient to prove
   LevelDB format compatibility?
4. After `WriteBatchReader::Open` validates a batch, should MemTable reparse
   every tag and length during insertion?
5. **Derive:** encode 16384 by dividing into seven-bit groups. Explain the
   extra byte relative to 16383.
6. **Parser proof:** design a length-prefixed consume operation with a local
   cursor. Show what happens when its prefix is valid but its payload is
   one byte short.
7. **Counterexample:** construct an encoder and decoder that agree with
   each other but disagree with the fixed32 golden example.
8. **Transfer:** design a format extension containing an unknown tag and
   explain whether an old reader can skip it safely. Which framing
   information is necessary before that policy is possible?

<details>
<summary>Answers</summary>

1. `80 01`.
2. Sequence 9's deletion sorts first. Sequence takes precedence over kind.
3. An encoder and decoder can share the same wrong convention.
   Independent golden bytes or a reference implementation are needed.
4. No. The checked boundary establishes the framing invariant; insertion uses
   the trusted representation while preserving its storage lifetime.
5. Its groups are 0, 0, and 1, so the bytes are `80 80 01`. Two payload
   groups hold only fourteen bits.
6. The result is an error and the caller's cursor remains unchanged.
   Advancing after the prefix alone breaks the compound consume contract.
7. Big-endian emission and matching big-endian decoding round-trip, but
   produce `12 34 56 78` instead of `78 56 34 12`.
8. A skippable record needs a trusted extent or another unambiguous framing
   rule. Guessing an unknown payload length cannot safely find the next tag.

</details>

## Source tour

| Entry point | Interface or symbol | Question to answer | Representative evidence |
|---|---|---|---|
| [`bytes.h`](../../include/modern_leveldb/base/bytes.h) | `ByteView`, mutable views, conversions | Which object owns the bytes behind this view? | [`bytes_test.cc`](../../tests/unit/base/bytes_test.cc) |
| [`coding.cc`](../../src/base/coding.cc) | fixed/varint encode and consume | Which failures leave the input unchanged? | [`coding_test.cc`](../../tests/unit/base/coding_test.cc) |
| [`coding_internal.h`](../../src/base/coding_internal.h) | trusted fixed words and unsigned varints | Which caller proved the complete word/varint and stable backing? | [`coding_test.cc`](../../tests/unit/base/coding_test.cc), [`table_test.cc`](../../tests/unit/table/table_test.cc) |
| [`internal_key.cc`](../../src/format/internal_key.cc) | `InternalKey`, `LookupKey`, comparator | How do sequence and kind affect physical ordering? | [`internal_key_test.cc`](../../tests/unit/format/internal_key_test.cc) |
| [`write_batch.cc`](../../src/format/write_batch.cc) | `EncodedWriteBatch`, `WriteBatchReader` | Where does external validation end and trusted iteration begin? | [`write_batch_test.cc`](../../tests/unit/format/write_batch_test.cc) |

Then read [ADR-0012](../adr/0012-internal-key-format.md),
[ADR-0016](../adr/0016-write-batch-format.md), and the owned-batch decisions
in [ADR-0060](../adr/0060-leveldb-write-path-parity.md).
