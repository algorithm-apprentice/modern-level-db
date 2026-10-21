# ADR-0081: Compatible Read Fast Paths

- Status: Implemented

## Context

The pinned LevelDB baseline is complete, but matching its format and read
semantics does not require retaining every intermediate key copy or repeated
comparison. A source audit of RocksDB revision
`a7f6f33cccc26685781ee7b9eee0f50e41153182` identified three bounded mechanisms
that fit the existing API and file format.

This experiment implements complete-key borrowing, prepared MemTable targets
with cross-level comparison reuse, and a bounded reverse replay cache. It does
not introduce a pinned-value API, whole-SST filters, another serialized index,
node/record coallocation, prefetch, or a different merging algorithm.
Existing owning `Get` still copies one selected value.

## Decision

### Borrow complete block keys

A checked entry with shared length zero already contains its full key.
`Block::Iterator` borrows those bytes instead of copying them into a string.
A non-null borrowed pointer also represents a complete empty key. Dependent
entries reconstruct their key in reusable string storage, reserving capacity
before committing the new position.

The scratch string keeps its capacity when a later complete key is borrowed.
Keys and values retain their movement-limited view lifetime. The caller still
keeps the comparator and backing Block storage alive; moving the owning Block
does not move its backing bytes.

Restart-key binary comparisons already borrowed block bytes before this
change. The removed copies are in final positioning and sequential entry
parsing. Index blocks written by this engine use restart interval one, but
the reader continues to accept general checked prefix-compressed entries.

### Search with prepared targets

Skip-list iterator seek accepts a heterogeneous target when the comparator
supports `Compare(Entry, Target)`. The target is borrowed only during the call.
MemTable reads pass the existing internal-key view, so each comparison decodes
only the stored record's length prefix. MemTable iterators no longer create a
temporary length-prefixed seek record.

A search remembers the most recent node known not to precede the target.
Descending a level can reuse that comparison because the target and published
record are unchanged. This is a local search result, not a cache across calls.
Insertion still collects every level's predecessor. Arena ownership,
one-writer synchronization, and acquire/release link publication are unchanged.

### Cache one checked reverse replay

An uncached reverse step replays from a preceding restart. When it visits
multiple predecessors, it can retain their positions, restart state, values,
and keys. Full keys continue to borrow Block bytes; dependent keys are copied
once into iterator-owned cache storage. Cached reverse steps borrow those
decoded key bytes rather than reconstructing or copying them again.

The cache is allocated lazily. Point reads, ordinary forward scans, and
single-predecessor reverse replays do not allocate its payload. Cache ownership
adds iterator bookkeeping; it is not a claim of zero overhead on other paths.

Descriptor and copied-key payload is admitted only while it fits
`max(4 KiB, encoded entry-region size)`. Allocator capacity overhead is
additional. If a replay exceeds the budget, it discards the partial selection
and finishes the ordinary checked replay. This prevents a compact block with
large shared prefixes from requiring a collection of arbitrarily expanded
keys. Allocation exceptions still propagate; budget admission is not an
error-recovery path.

A completed replay activates an explicit reverse epoch. Only consecutive
`Prev` calls can reuse that context. Positioning, forward movement, and
invalidation end it without freeing bytes still borrowed by the current key.
The cache remains owned until the iterator is destroyed or safely rebuilt.

An entry offset alone is not a sufficient cache identity under lazy malformed
restart interpretation: two checkpoints can reach the same suffix entry with
different key histories. The epoch preserves the newly established checkpoint
instead of reviving a previous interpretation. Reaching the first cached
entry falls back to the usual checked predecessor-restart search.

## Validation

The implementation retains the checked entry decoder, minimum internal-key
length boundary, public `Status` protocol, corruption-view invalidation, and
recovery on a later positioning operation.

Focused RED-to-GREEN cases establish:

- Full-key pointer identity for owned/borrowed backing, empty and extended
  headers, long/binary keys, positioning, and Block moves.
- Prepared targets that cannot convert to stored entries.
- At most one comparison of each visited node within a lower-bound search.
- Distinct borrowed decoded keys during reverse replay, direction changes,
  and moves of the backing owner.
- Bounded admission for large shared prefixes and dense empty-key entries,
  with complete checked fallback traversal.
- Corruption visibility after cached steps and checkpoint changes.

The literal replay-context regression failed against the first cache
implementation. A separate probe compiled with the exact frozen pre-cache
header and library confirmed the original predecessor independently.
The corrected implementation passes 970 unit/public cases and 18 native
compatibility, crash, and database-model cases on Windows x64. The format
fuzzer consumer also compiles; this is not a claim of a completed fuzz campaign.

## Measurement protocol and provenance

Four independently frozen Release libraries represent:

| Variant | Production revision | Difference |
|---|---|---|
| B | `06cb3ad` | Existing owned decompression output and single-call read CRC |
| K | `0a84284` | Complete-key borrowing |
| S | `f6acff0` | Prepared MemTable targets and comparison reuse |
| R | `080ad9b` | Bounded reverse cache with corrected replay epochs |

Both timing objects use only the unchanged public headers. The existing
Get/forward-scan/retained-Seek object is reused without recompiling its loops.
One additional common object implements nonempty-MemTable and reverse reads.
Every variant links the same frozen CRC32C, Snappy, Zstd, and native-reference
archives. The native archive supports the existing checksum equivalence probe;
there is no new native timing comparison.

All 54 normalized core compiler commands match across the variants:
MSVC x64 Release, `/O2 /Ob2 /DNDEBUG /MD`. Six pairs alternate before/after
order. Only the runner and inherited experiment processes use logical CPU 2.
Accepted SST background work is queued until timing ends.

SST cases copy the same hash-verified closed image: one million 11-byte keys,
256-byte values, mapped reads, checksums, and 8 MiB or 512 MiB decoded-block
cache. Each process validates complete data before and after timing, with
fingerprint `2355815558` and unchanged per-level file topology.
MemTable cases create the same million records outside timing with a 1 GiB
write-buffer threshold, require no SST/immutable table/background work, and
check unchanged arena charge.

Each sample performs one million Get/Seek operations, eight complete forward
scans, or four complete reverse scans. Full reverse validation also compares
every returned key/value with an independent point lookup outside timing.
There is no adaptive calibration or favorable extra round.

Review found the replay-context defect after the initial final binary
`3a8bc56` had produced 90 complete samples. The run was stopped, with one
incomplete process. All 42 samples belonging to old-final comparisons are
superseded, regardless of their timings. The 24 B-to-K and 24 K-to-S samples
remain valid because those exact libraries and timing objects are unchanged.
The corrected run collected all 12 S-to-R reverse samples and all 48 B-to-R
integrated samples anew: 60 fresh plus 48 unchanged samples, or 108 accepted
observations. Independent recomputation checked every accepted process output,
paired-round identity, median, cost ratio, input hash, and operation count.

Raw plans, hashes, commands, complete/superseded samples, regression evidence,
and corrected results are retained under
`build\rocksdb-read-optimizations-20261021`; the corrected accepted matrix is
under `corrected\measurements`.

## Measured outcomes

Lower costs are better. Lookup rows use microseconds per operation; scan rows
use milliseconds per complete million-record pass. Elapsed and process-CPU
deltas compare the medians of six per-process means. Faster-pair counts compare
the two executions within each round, not a statistical-significance test.

| Contrast | Workload | Before | After | Elapsed cost | CPU cost | Faster pairs |
|---|---|---:|---:|---:|---:|---:|
| B -> K | Get, 8 MiB cache | 7.169 us | 6.998 us | -2.39% | -2.74% | 4/6 |
| B -> K | Get, 512 MiB cache | 5.453 us | 5.308 us | -2.67% | -3.02% | 5/6 |
| K -> S | MemTable Get | 2.603 us | 2.619 us | +0.62% | -0.90% | 3/6 |
| K -> S | Retained MemTable Seek | 2.501 us | 2.451 us | -1.99% | -2.80% | 5/6 |
| S -> R | Reverse scan | 459.822 ms | 399.075 ms | -13.21% | -12.82% | 6/6 |
| B -> R | Get, 8 MiB cache | 7.126 us | 6.799 us | -4.59% | -4.86% | 6/6 |
| B -> R | Get, 512 MiB cache | 5.293 us | 5.042 us | -4.74% | -4.87% | 5/6 |
| B -> R | Forward scan | 261.645 ms | 260.426 ms | -0.47% | -1.12% | 4/6 |
| B -> R | Retained SST Seek | 7.217 us | 6.959 us | -3.57% | -3.91% | 6/6 |

Reverse replay caching gives the clearest measured reduction. Integrated
cache-pressure Get and retained SST Seek also improve in every pair.
The resident Get result improves in five pairs. Forward scan's small mixed
change does not establish a substantial benefit. Prepared MemTable Get has
no stable elapsed-time gain; retain that counterexample rather than treating
fewer comparisons as proof of lower latency.

The isolated and integrated phases ran at different times. Their percentages
are not additive, and their difference is not a causal decomposition. The
SST image has no searchable MemTable records, so its Get result is not evidence
of a nonempty-MemTable speedup.

These warm single-foreground-thread results are per-process mean costs, not
tail latencies. They do not establish cold-I/O, concurrent-read, write-throughput,
or native Get-gap attribution.

## Source references

- [RocksDB block iterator](https://github.com/facebook/rocksdb/blob/a7f6f33cccc26685781ee7b9eee0f50e41153182/table/block_based/block.cc):
  complete-key borrowing and decoded reverse-entry caching.
- [RocksDB inline skip list](https://github.com/facebook/rocksdb/blob/a7f6f33cccc26685781ee7b9eee0f50e41153182/memtable/inlineskiplist.h):
  prepared targets and repeated upper-bound comparison avoidance.
- [Block implementation](../../src/table/block.cc) and
  [behavioral cases](../../tests/unit/table/block_test.cc).
- [MemTable implementation](../../src/memory/memtable.cc),
  [skip list](../../src/memory/skiplist.h), and
  [search cases](../../tests/unit/memory/skiplist_test.cc).
