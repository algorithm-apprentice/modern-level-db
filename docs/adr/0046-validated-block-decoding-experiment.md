# ADR-0046: Invariant-Based Decoding of Validated Blocks

## Status

Accepted design. Merge this design before its separate experiment.

## Context and experiment selection

The user requested measured optimization experiments, with throughput as well
as average time, and specifically asked that internal hot paths rely on proven
loop invariants rather than repeat defensive validation.

The first proposed experiment was lazy point-read file selection. Before any
production change, the investigation confirmed a more direct opportunity to
exercise that principle in `Block`: ADR-0022 already establishes full
creation-time validation and immutable contents, but both restart-key lookup
and iterator entry parsing reuse the checked decoder afterward. Lazy file
selection, merge-cursor caching, and compaction key-buffer reuse remain
separate later candidates.

The current checked entry decoder calls `ConsumeVarint32` three times, handles
three fallible results, and rechecks the combined key/value extent. Those
checks are needed while validating external bytes, but their failure cases
have already been excluded when an iterator reads a successfully created
immutable block.

Existing post-CRC profiles identify entry decoding in the read path. The
recent single-foreground-thread read baseline is approximately 1.68 million
Get/s versus 2.53 million for the pinned LevelDB at 4,096 records, and 0.60
million versus 0.92 million at 65,536 records. These motivate investigation;
they are not the admission baseline and do not establish how much of the gap
this candidate can recover.

LevelDB checks entries lazily; its decoder therefore retains bounds/error
handling. RocksDB also has multiple format-specific decoding paths. Do not
copy an unchecked implementation on the assumption that our invariants are
the same. This experiment exploits the stronger invariant Modern LevelDB
already promises, without adopting another persistent format or weakening
the corruption-detection contract.

## Decision

### Validation belongs where it establishes a fact

Retain runtime checks at the boundary where bytes or operation results can
be invalid: file offsets/lengths, short reads, CRC, compression type and size,
decompression errors, and complete block structure/key-order validation.
Do not infer structural validity merely from a matching checksum.

After successful validation, internal loops may rely on established
invariants. Keep debug assertions for programming preconditions instead of
reconstructing recoverable errors for impossible internal states.
Termination tests and decoding a varint's continuation bit are necessary
algorithmic work, not redundant validation.

This is not permission to remove checks from unrelated APIs or to suppress
real I/O errors. An unchecked helper requires identified callers and an
ownership/state argument proving its preconditions.

### Proof boundary and loop invariants

`Block::Create` is the only successful construction path. Its checked
`Validate` establishes:

- Every entry header terminates within the entry region. Each of its three
  varint32 values consumes at most five bytes.
- Key-delta and value extents fit within the entry region without offset
  truncation; the final entry ends at the restart array.
- The first restart is zero; every other restart denotes an actual entry
  with `shared == 0`, in strictly increasing offset order.
- The shared prefix never exceeds the preceding reconstructed key.
- Reconstructed keys strictly increase under the retained comparator.

The block owns its buffer, exposes no mutation API, and cannot be copied or
assigned. Moving it preserves buffer addresses. Callers must not mutate
retained aliases to transferred storage; iterators require the owning block
and comparator to remain alive, as they already do.

For each iterator entry decode, `next_` is a validated entry boundary before
`entries_end`. Seeking begins at a validated restart with an empty key and
zero shared length. Forward stepping uses the previous entry's validated
end and reconstructed key. Reverse stepping rebuilds from an earlier
validated restart. `ParseNextEntry` retains its end-of-region test, and
empty blocks never enter the trusted decoder.

`Layout::RestartKey` likewise decodes only a validated, nonempty restart
entry. A restart lookup in an empty block is not needed by the seek
algorithm. Assertions document these internal preconditions.

### Narrow implementation

Keep the existing checked decoder and `Block::Validate` unchanged.
Add private, clearly named validated-varint and validated-entry decoding
helpers in `src/table/block.cc`. They return values directly, not
`Result`/`Error`, and are used only by:

1. `Block::Layout::RestartKey`.
2. `Block::Iterator::ParseEntry`.

The trusted varint helper reads bytes until the continuation bit clears.
Its shift sequence is 0, 7, 14, 21, 28 at most, as established by validation.
Use unsigned 32-bit arithmetic. **Do not impose canonical encodings or
reject excess terminal payload bits**: the existing `ConsumeVarint32`
matches LevelDB by discarding bits above bit 31 in the fifth byte.
Accepted nonminimal and terminal-bit encodings must decode identically.

Compute extents and pointer differences as `std::size_t`; do not introduce
a 32-bit sum of key and value lengths. Return views into the same immutable
storage and preserve key/value lifetimes and all iterator positions.

Do not add global unchecked decoding APIs, assume macros, speculative
overreads, native-endian loads, new dependencies, or cached decoded-entry
tables. Retain key-buffer reservation before iterator state mutation, so an
allocation failure leaves the iterator consistent under the existing
exception contract.

## Verification before timing

Strengthen tests while the old implementation is still present, then run
them unchanged against the candidate:

- Valid entry headers with one through five bytes per varint, including
  nonminimal encodings and accepted excess fifth-byte payload bits.
- Both restart and prefix-compressed entries with binary/empty data.
- Forward/backward traversal, seek, direction changes, end positions,
  empty blocks, and iterators surviving a block move.
- The existing randomized ordered-model oracle with multiple restart
  intervals and forward/reverse comparators.
- Rejection of truncated, unterminated, oversized, invalid-restart,
  invalid-shared-prefix, and incorrectly ordered blocks by the unchanged
  creation boundary, before any trusted decode can occur.

Use the existing format/differential, sanitizer, fuzz, platform, and
changed-code coverage gates. In particular, accepted noncanonical inputs
must not create undefined shifts, overreads, or different iterator output.
Inspect optimized code/call stacks to verify the iterator no longer invokes
the fallible generic decoder; assertions alone are not proof of a faster path.

## Frozen measurement and admission

After this design merges, build and retain a clean baseline executable,
configure header, compile commands, revision, and SHA-256. Build the
candidate with the identical profiling preset and preserve its identity.
The production comparator, CRC selection, block validation policy, cache,
codecs, and all benchmark workloads/options stay unchanged.

Before collection, fix three paired rounds. In each round run all eight
Modern read-family cases (`readrandom`, `readmissing`, `scan`, `seek_reuse`,
each at 4,096 and 65,536 records), with three framework repetitions and a
0.3-second calibration minimum per fresh process. Use baseline then
candidate in rounds one and three, reversing order in round two.
This is 48 processes and nine individual samples per variant/case.

Also collect one candidate-executable LevelDB reference process for each
random-read size after each round, with the same repetition/time settings.
These six processes give current reference context, not the admission metric.
Its optional external hardware CRC remains disabled and its native mmap
behavior unchanged. Do not call this a comparison against the fastest
possible LevelDB or a RocksDB measurement.

For each individual sample, throughput is `1e9 / wall_ns_per_item`;
for point reads this is Get/s. Aggregate individual samples by median, not
framework mean/median rows. Require all of the following:

- At least **5% higher median Get/s** for `modern/readrandom/4096` than the
  frozen original, and positive improvement in each paired round.
- No other Modern read-family case loses more than **5% aggregate median
  throughput**, or loses more than 5% in at least two of the three rounds.
- Correctness and all selected build/CI gates pass.

The primary is cache-fit Get because repeated decoding is on its measured
CPU path while file/checksum/compaction waits are minimized. Cache-pressure
Get and scans are explicit controls, not assumed to share the same gain.
Report average wall time and process CPU as supporting metrics. These are
single-foreground-thread measurements, not request P99 or concurrent
saturation throughput.

Do not add favorable rounds, weaken thresholds, change data sizes, or
reduce validation after seeing results. If admitted, capture matched
baseline/candidate cache-fit Get CPU intervals afterward for attribution;
profiled timings are not the speedup claim. If rejected, restore production
code and retain applicable tests and the recorded outcome.

## Scope and delivery

This is one independent decoding experiment. It does not remove the full
creation-time validation pass or optimize candidate selection, comparator,
cache, mmap, API ownership, concurrency, or compaction policy.
Obtain a bounded Sol design review, merge the design-only PR first, then
review and deliver the separate measured implementation or rejection PR.

## References

- [ADR-0022 block validity and iteration contract](0022-sstable-block-format.md)
- [Checked varint implementation](../../src/base/coding.cc)
- [LevelDB checked block decoder](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/block.cc)
- [RocksDB block iterators](https://github.com/facebook/rocksdb/blob/abeebd9630f11bd08c28b7bd43c7bdfc62050654/table/block_based/block.h)
- [ADR-0042 read profiling](0042-benchmark-profiling-foundation.md)
- [ADR-0043 rejected comparator experiment](0043-profile-guided-bytewise-comparison.md)
