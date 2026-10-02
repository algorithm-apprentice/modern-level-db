# 05. Tables, Filters, and Caches

[Learning path](README.md) | Next: [Reads and iterators](06-reads-and-iterators.md)

Prerequisites: [formats](02-bytes-and-formats.md) and
[durable installation](04-wal-and-recovery.md).

## An SSTable is more than a sorted array

An SSTable, or sorted string table, is an immutable file:

```text
data blocks
optional filter block
metaindex block
index block
48-byte footer
```

Stored blocks have five-byte trailers: one compression-type byte and four
bytes of masked CRC32C. A block handle stores its offset and size; its size
excludes the trailer.
The footer contains handles for the metaindex and index, padding, and the
LevelDB table magic number.

The index maps separator keys to data-block handles.
The metaindex locates optional metadata such as the filter block.
Immutability lets readers cache these structures without coordinating
in-place page updates.

## Prefix compression and restart points

Sorted keys commonly share prefixes. A block entry stores:

```text
varint32(shared prefix length)
varint32(non-shared key length)
varint32(value length)
key suffix
value
```

For the teaching keys `"user:001"` and `"user:002"`:

```text
first key:  shared = 0, suffix = "user:001"
second key: shared = 7, suffix = "2"
```

The second key is reconstructed from the first seven bytes of the previous
key plus `"2"`. Actual database keys also contain internal-key trailers.

If every key depended on its predecessor, seeking near the end would
require decoding from the beginning.
Periodic **restart points** store keys with shared length zero.
A fixed32 offset array and count at the end of the block locate them.

Seek searches restart keys and scans forward within a region.
The default data restart interval is sixteen; the table's index block uses
interval one. A smaller interval costs space but reduces reconstruction
work. A larger interval saves restart metadata but can increase seeks.

`Block::Create` checks the restart-array region, not every entry.
Data iterators check bounds and reconstructed keys lazily.
Table-open validation walks all index entries. When a filter policy is
configured, it also reads and validates the metaindex entries.
A valid block envelope is not a certificate that every data entry is valid.

## Index separators

An index key need not repeat the complete last data key if a shorter key
still separates this block from the next one.
The comparator's separator/successor helpers create valid routing bounds.

Their obligation is ordering correctness, not maximum compression.
Custom comparators may leave keys unchanged.
An invalid shortened separator can route reads to the wrong block even if
all block checksums are correct.

## Bloom filters

A Bloom filter answers a narrow question:

```text
Definitely absent? -> skip the data-block read
Possibly present?  -> perform the real lookup
```

For valid construction and matching key semantics, it can have false
positives but not false negatives.
With `n` keys, `m` bits, and `k` probes, the usual approximation is:

```text
false-positive probability ~= (1 - exp(-k * n / m))^k
```

Modern uses LevelDB's seeded hash and repeated probes from one hash plus a
rotated delta. The probe count is `floor(bits_per_key * 0.69)`, clamped
between one and thirty. Ten bits per key gives six probes and approximately
a one-percent false-positive rate for a sufficiently large filter.

Filters cover user keys, not sequence-bearing internal keys. Otherwise a
lookup at another snapshot could incorrectly reject a stored version.
Deletion entries also contribute their keys.

The filter block associates filters with 2 KiB ranges of data-block starting
offsets. This is not necessarily one filter per data block.
Bloom filtering is optional and disabled unless `bloom_bits_per_key` is set.

If a custom comparator treats different bytes as equal, the byte-based
filter can violate lookup semantics. Enabling it requires comparator
equality to imply byte equality.

## Block compression

The current engine and pinned LevelDB both support None, Snappy, and Zstd.
Compression is selected independently per stored block.
Incompressible blocks are stored raw.

The exact admission rule is:

```text
compressed_size < raw_size - floor(raw_size / 8)
```

For 4096 raw bytes, 3583 compressed bytes are accepted and 3584 are not.
The strict inequality is part of the behavior worth testing.

Checksums protect the stored representation and compression type.
Reading verifies those bytes before decompression.
Decompression also validates its format and declared output size.
Corruption must become an error, not a plausible empty block.

## Three different caches

| Cache | What it retains |
|---|---|
| Table cache | Open table objects, their files, indexes, and filter metadata |
| Block cache | Independently owned decoded data blocks |
| Operating-system page cache | File pages maintained by the kernel |

They solve different problems. A table-cache hit is not necessarily a
data-block-cache hit, and a block-cache miss is not necessarily physical
device I/O.

The internal LRU cache has sixteen shards.
Handles pin entries while they are used; only unpinned LRU entries are
eviction candidates.
Replacement or erasure removes cache discoverability, not the validity of
an already retained handle.

Capacity is an eviction budget, not a strict process memory limit.
Pinned entries can exceed it, and per-shard rounding and ownership outside
the cache also matter. The public facade currently creates an internal
8 MiB block cache without exposing a capacity option.

`fill_cache=false` avoids admitting newly read data blocks.
It does not disable existing cache hits, the table cache, or the OS cache.

## mmap and borrowed blocks

On supported POSIX systems, default table reads can use an exact-size
immutable mapping.
An uncompressed mapped block can borrow its file bytes instead of copying
them. Such a block is not independently admitted to the block cache;
the table/file lifetime protects it.

Compressed mapped blocks still need owned decompressed output and can be
cached. Copied uncompressed blocks also have owned storage.

mmap avoids a copy, not page faults or all I/O.
Mapped storage faults can terminate the process with `SIGBUS`.
`allow_mmap_reads=false` selects copied reads when typed read errors are
required. External modification or truncation of live database files is
unsupported.

## Source tour

| File | Focus |
|---|---|
| [`block_builder.cc`](../../src/table/block_builder.cc) | Prefix encoding and restart offsets |
| [`block.cc`](../../src/table/block.cc) | Restart search, lazy decoding, and invalidation |
| [`table_builder.cc`](../../src/table/table_builder.cc) | Pending index entries and final file layout |
| [`bloom_filter.cc`](../../src/table/bloom_filter.cc) | Probe calculation and matching |
| [`table.cc`](../../src/table/table.cc) | Index/filter lookup and block ownership |
| [`sharded_lru_cache_core.cc`](../../src/cache/sharded_lru_cache_core.cc) | Pins, LRU membership, and eviction |

Follow with [ADR-0022](../adr/0022-sstable-block-format.md),
[ADR-0023](../adr/0023-sstable-filter-blocks.md),
[ADR-0039](../adr/0039-sstable-block-compression.md), and
[ADR-0056](../adr/0056-leveldb-table-mmap-parity.md).

## Self-check

1. Why does increasing the restart interval potentially slow a seek?
2. Can a Bloom "possibly present" result prove that a key exists?
3. Why can a pinned cache entry survive erasure?
4. Does an mmap read necessarily come from RAM without a storage access?

<details>
<summary>Answers</summary>

1. More dependent entries may need reconstruction between restart points.
2. No. It is permission to continue lookup, not the lookup result.
3. The handle retains ownership even after the entry is no longer found
   through the cache's key index.
4. No. The kernel may need to fault in file pages from storage.

</details>
