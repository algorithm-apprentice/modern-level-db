# 05. Tables, Filters, and Caches

[Learning path](README.md) | Next: [Reads and iterators](06-reads-and-iterators.md)

Prerequisites: [formats](02-bytes-and-formats.md) and
[durable installation](04-wal-and-recovery.md).

## Lesson map

| Question | Answer |
|---|---|
| What problem is this lesson about? | Storing ordered immutable data so reads avoid scanning whole files |
| Who consumes this layer? | Flush, compaction, TableCache, point reads, and iterators |
| What interface matters first? | TableBuilder Add/Finish, Table Open/Get/Iterator, filter queries, and cache handles |
| What implementation will we open? | Blocks, restart points, indexes, Bloom filters, compression, caching, and mapped files |
| What should you be able to do afterward? | Trace a key from an index lookup to a decoded data block and explain each cache/lifetime involved |

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
The reader checks contiguous contents plus the type byte in one CRC32C
calculation. The writer extends the checksum across its separate contents and
type buffers.
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

The checked entry decoder returns the key-delta pointer and three lengths.
Private iterator loops advance with a boolean position result and latch
corruption locally; the public movement call returns the corresponding
`Status`. End-of-block is not an error. Key capacity is reserved before
committing a reconstructed entry, preserving a consistent position if
allocation throws.

An entry with shared length zero already contains its complete key. The
iterator borrows those bytes directly, including an empty complete key.
Only dependent entries materialize a key in reusable iterator storage.
Switching back to a complete key retains that storage's capacity for later
reconstruction. Both representations have the same movement-limited view
lifetime, and moving the owning block preserves its backing addresses.

Entries have no backward links. An uncached reverse step replays from a preceding
restart and caches the checked entries, decoded keys, and borrowed values.
Consecutive steps within that cached interval reuse those positions and key
bytes instead of decoding the same prefixes again. Positioning and forward
movement end that checked replay context, but keep its backing bytes alive
until the current borrowed key has been consumed. A subsequent reverse step
replays from its own checkpoint; a failed replay does not publish a partial cache.
The cache is allocated lazily only when replay reaches multiple predecessors,
so point reads, forward scans, and single-predecessor replays do not allocate it.
Cached key views still expire on movement, just like forward key views.
Descriptor and copied-key payload is budgeted to the greater of 4 KiB and
the encoded entry region's size; allocator capacity overhead is additional.
If prefix expansion or dense entries exceed that budget, checked replay
continues without publishing the partial cache. A compact encoded block
therefore cannot require an unbounded collection of expanded reverse keys.

## Index separators

An index key need not repeat the complete last data key if a shorter key
still separates this block from the next one.
The comparator's separator/successor helpers create valid routing bounds.

Their obligation is ordering correctness, not maximum compression.
Custom comparators may leave keys unchanged.
An invalid shortened separator can route reads to the wrong block even if
all block checksums are correct.

### A checked index is a reusable proof

Follow one index value across its lifetime:

```text
Open: validate both complete handle varints and their file bounds
    -> retain the immutable index and its backing owner
Get or iterator load: decode that validated offset/size pair privately
    -> keep checking data entries lazily as they are reached
```

The trusted handle decoder relies on those same stable bytes, not on the
belief that disk data is usually valid. A complete index handle says nothing
about every entry in the selected data block. Changing the backing bytes or
releasing their owner would invalidate the proof.
See [formats and trusted loops](02-bytes-and-formats.md#checked-boundaries-and-trusted-loops).

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

The legacy layout uses LevelDB's seeded hash and repeated probes from one hash plus a
rotated delta. The probe count is `floor(bits_per_key * 0.69)`, clamped
between one and thirty. Ten bits per key gives six probes and approximately
a one-percent false-positive rate for a sufficiently large filter.

Filters cover user keys, not sequence-bearing internal keys. Otherwise a
lookup at another snapshot could incorrectly reject a stored version.
Deletion entries also contribute their keys.

The legacy filter block associates filters with 2 KiB ranges of data-block
starting offsets. This is not necessarily one filter per data block, and
selecting that filter requires an index result first.

The default built-in bytewise path instead writes one versioned full-table
filter. It rejects an absent SST candidate before index seek, not just before
the data read. Its probes stay within a selected 64-byte region, improving
locality without claiming that a positive result proves presence. The writer
collects key hashes while building the table; the reader never scans the
table to rebuild a filter. Its bytes are distinct optional metadata, so old
readers safely ignore it.

Both layouts use the exact integer probe formula and cover complete user
keys, including deletion entries and duplicate versions.
`bloom_bits_per_key` defaults to ten; explicit `std::nullopt` disables it.
Automatic layout disables filters for a custom comparator because equality
between different byte strings would make exclusion unsafe. Explicit layout
selection promises comparator equality implies byte equality. Comparator
names are not evidence for that promise.

See [ADR-0086](../adr/0086-persisted-full-table-filters.md) for the wire-format
and ownership proofs; use [API and options](../reference/api-and-options.md)
for current selection behavior.

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

The codec writes directly into an overwrite-allocated byte array. A small
pointer/length owner transfers that allocation into `BlockContents`, without
converting it to a vector or copying the decoded bytes. Already-owned raw
vectors and borrowed mapped bytes keep their separate zero-copy paths.
Moving a block transfers ownership, not the backing bytes.

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

Point-read reader reuse distinguishes another pair of lifetimes. A mapping
pin protects both the cache entry and its Table. A temporary shared Table
owner protects only the reader/file. The optional 64-slot per-thread weak
hint array owns neither while idle; a hit locks a reader just for that Get.
Fallback reads keep their real mapping pin. A monotonic per-shard open-event
gate disables hints before pressure can exceed its known ceiling, and an
eviction generation prevents an old live reader from becoming a stale hint.
Past skipped LRU promotions cannot be reconstructed at that handoff.
Ownership mode is also immutable: closing a gate does not turn Shared tables
back into Unique tables. This explains why avoiding recency maintenance can
help a resident concurrent workload without making reader reuse a universal
win under pressure. The default remains off, and the matched disabled controls
also do not establish a universally cost-free feature.

Capacity is an eviction budget, not a strict process memory limit.
Pinned entries can exceed it, and per-shard rounding and ownership outside
the cache also matter. With no supplied cache the engine creates an 8 MiB
cache. The public capacity factory allows a different shared budget; the
separate checked options factory also selects LRU or CLOCK. Automatic stays
LRU, and retained block charge is not allocator metadata or RSS.

CLOCK separates replacement preference from value lifetime. A visited bit
approximates recency without moving a list on every hit; atomic pins still
protect the complete immutable key and value. Stable slot addresses alone
are not enough: a reused position can represent another generation, so a
reader must acquire its pin before checking every identity byte. An erased
last-pin cleanup must recheck eligibility after obtaining writer serialization.
Bounded contention or index refresh may cause a cache miss, never a wrong-key
hit or logical database absence. Evaluate both maintenance cost and the
resulting hot/scan/shift miss curves instead of inferring a universal win
from cheaper hits.
The own CLOCK measurements demonstrate this distinction: resident maintenance
and Get can improve while pressured concurrent reads and shifted hot sets
regress. Automatic therefore retains LRU. Separate original/facade-LRU controls
also matter: adding policy dispatch is not automatically free.

`fill_cache=false` avoids admitting newly read data blocks.
It does not disable existing cache hits, the table cache, or the OS cache.

A positioned table iterator also retains its current data block and cursor.
If another positioning operation selects the same encoded block handle, it
reuses them without a cache lookup or file read. This iterator-local reuse
works without a block cache; exhaustion or a returned error releases the
retained block.

## Retire under the mutex, reclaim after it

An LRU entry has two independent relationships:

```text
indexed membership -> lookup can find the entry; cache owns one reference
external pin       -> a handle owns another reference and may use the value
```

Replacement, erasure, and eviction first change membership under the shard
mutex. They remove the hash-table/list links, subtract the indexed charge, and
release the cache reference. If that was the final reference, they put the
entry on a local retired chain instead of destroying it immediately:

```text
under shard mutex:
    remove mapping and list links
    update charge and references
    zero references -> append unreachable entry to retired chain

after unlocking:
    invoke the typed value deleter
    destroy the entry
    release its allocation
```

The typed value is caller-owned application or engine state. Its destructor
is external code from the core cache's point of view and can call back into
the same cache. Running it under a non-recursive shard mutex could deadlock.
Even a recursive mutex would expose partially completed list, hash, or charge
transitions to the reentrant operation. The retired entry is therefore made
unreachable and the shard invariant is restored before destruction begins.

`Insert`, `Erase`, and final pin release all use this two-phase pattern. If an
erased entry still has a pin, erasure removes membership but does not retire
the value. The last handle later decrements the final reference under the
mutex, then reclaims it after unlocking.

Reentrancy can produce a result that looks surprising but is ownership-safe.
During replacement, the old value's deleter may erase the newly installed
mapping before `Insert` returns. A later `Lookup` can therefore miss, while
the `Insert` result still contains a valid pin to the replacement value.
Membership changed; retained ownership did not. The test
`ReplacementReclamationCanEraseTheNewMappingBeforePinReturn` fixes this exact
behavior.

Cache destruction has a stronger precondition: its owner has already stopped
concurrent cache operations and released every handle. It can detach all
remaining entries and then run their deleters without promising that a cache
being destroyed is reentrant-safe.

## Mapped files and borrowed blocks

On supported native platforms, default table reads can use an exact-size
immutable mapping: POSIX uses `mmap`, while Windows uses
`CreateFileMappingW` and `MapViewOfFile`.
An uncompressed mapped block can borrow its file bytes instead of copying
them. Such a block is not independently admitted to the block cache;
the table/file lifetime protects it.

Compressed mapped blocks still need owned decompressed output and can be
cached. Copied uncompressed blocks also have owned storage.

Mapping avoids a copy, not page faults or all I/O.
POSIX storage faults can terminate the process with `SIGBUS`; Windows mapped
access can fail through the operating system's in-page/access-fault path.
`allow_mmap_reads=false` selects copied reads when typed read errors are
required. External modification or truncation of live database files is
unsupported.
For mapping admission, rollback, native handle ownership, and teardown order,
see [lesson 12](12-native-filesystem-lifecycles.md).

## Source tour

| Entry point | Interface or symbol | Question to answer | Representative evidence |
|---|---|---|---|
| [`block_builder.cc`](../../src/table/block_builder.cc) | `BlockBuilder::Add`, `Finish` | How do prefix compression and restart offsets preserve seekability? | [`block_test.cc`](../../tests/unit/table/block_test.cc) |
| [`block.cc`](../../src/table/block.cc) | `Block::Create`, iterator seek | Which checks establish safe lazy decoding? | [`block_test.cc`](../../tests/unit/table/block_test.cc) |
| [`table_builder.cc`](../../src/table/table_builder.cc) | `Add`, `Finish`, pending index entry | How are data, filter, metaindex, index, and footer ordered? | [`table_builder_test.cc`](../../tests/unit/table/table_builder_test.cc) |
| [`bloom_filter.cc`](../../src/table/bloom_filter.cc) | `CreateFilter`, `KeyMayMatch` | Why can a filter reject but not prove presence? | [`bloom_filter_test.cc`](../../tests/unit/table/bloom_filter_test.cc) |
| [`table.cc`](../../src/table/table.cc) | `Open`, `Get`, data-block lookup | Which objects own or borrow index, filter, and block bytes? | [`table_test.cc`](../../tests/unit/table/table_test.cc) |
| [`sharded_lru_cache_core.cc`](../../src/cache/sharded_lru_cache_core.cc) | `CommitAdmission`, `FinishErase`, `Retire`, `DeleteRetired`, `Release` | How are membership and references committed under the mutex while a reentrant value deleter runs after unlocking? | [`sharded_lru_cache_test.cc`](../../tests/unit/cache/sharded_lru_cache_test.cc) |

Follow with [ADR-0014](../adr/0014-sharded-lru-cache.md),
[ADR-0022](../adr/0022-sstable-block-format.md),
[ADR-0023](../adr/0023-sstable-filter-blocks.md),
[ADR-0039](../adr/0039-sstable-block-compression.md), and
[ADR-0056](../adr/0056-leveldb-table-mmap-parity.md).

## Self-check

1. Why does increasing the restart interval potentially slow a seek?
2. Can a Bloom "possibly present" result prove that a key exists?
3. Why can a pinned cache entry survive erasure?
4. Does a mapped read necessarily come from RAM without a storage access?
5. Why is an uncompressed block that borrows mapped file bytes not inserted
   into an independently owning block cache?
6. Why does the cache retire a zero-reference entry under the shard mutex but
   invoke its value deleter only after unlocking?
7. After Open validates an index handle, can every entry in its data block
   use an unchecked decoder?

<details>
<summary>Answers</summary>

1. More dependent entries may need reconstruction between restart points.
2. No. It is permission to continue lookup, not the lookup result.
3. The handle retains ownership even after the entry is no longer found
   through the cache's key index.
4. No. The kernel may need to fault in file pages from storage.
5. Its bytes are owned by the table/file mapping. Caching it independently
   would require copying or otherwise retaining that owner.
6. The deleter is external code and may reenter the cache. The shard must first
   make the entry unreachable and restore its hash/list/charge invariants;
   otherwise destruction could deadlock or observe a partial mutation.
7. No. The proof covers the complete handle in stable, retained index bytes.
   It does not validate the data block's entries; their lazy checks remain.

</details>
