# ADR-0049: LevelDB-Style Point-Read Baseline

## Status

Accepted design. Merge this design before the separate diagnostic-tooling
change and before any read-path implementation experiment.

## Context

Modern LevelDB's point reads remain slower than the pinned Google LevelDB
reference after the accepted CRC32C and validated-decoder improvements. The
latest retained production-equivalent measurements in
[ADR-0047](0047-inline-iterator-key-experiment.md) report:

| Random-read case | Modern LevelDB | Google LevelDB | Modern wall time | Modern process CPU time |
|---|---:|---:|---:|---:|
| 4,096 records | 2,126,602 Get/s | 2,469,320 Get/s | 470.23 ns/Get | 470.18 ns/Get |
| 65,536 records | 664,323 Get/s | 973,277 Get/s | 1,505.29 ns/Get | 1,505.19 ns/Get |

These are warm, single-foreground-thread benchmark results. The workload
prepares and verifies the database before timing and does not purge the
operating-system page cache. The close wall/process-CPU times therefore
support a CPU, memory, page-cache, and system-call investigation; they do not
support calling the gap physical-device wait.

The warning-free cache-fit CPU capture retained by ADR-0047 assigns
overlapping inclusive sample weight of 63.73% to block seek, 33.42% to
internal-key comparison, 25.97% to entry parsing, 5.88% to block-cache lookup,
and 4.48% to file-candidate construction. A separate 65,536-record capture
assigns 16.83% self weight to `pread`, and overlapping inclusive weights of
16.29% to stored-block decoding, 14.11% to decompression, 10.88% to block
construction, 9.99% to full block validation, and 9.79% to block-cache
insertion.

Sample weight is not elapsed-stage latency, and inclusive weights cannot be
summed. In particular, a `pread` CPU sample is not the same as time waiting
for a storage device. The current tools establish useful directions but do
not yet separate file-access policy, cache behavior, validation work, and
result ownership well enough to assign the throughput gap.

The benchmark already aligns the material options that should remain fixed:

- Google LevelDB revision `7ee830d02b623e8ffe0b95d59a74db1e58da04c5`.
- Google Benchmark v1.9.5 at
  `192ef10025eb2c4cdd392bc502f0c852196baa48`.
- 4,096 or 65,536 records with 256-byte values.
- 8 MiB block cache, 64 KiB write buffer, 4 KiB data blocks, and restart
  interval 16.
- Snappy compression, no Bloom filter, and checksum verification enabled.
- One foreground benchmark thread and no operating-system page-cache purge.
- Modern LevelDB's pinned accelerated CRC32C implementation; the reference's
  optional external hardware CRC implementation remains disabled.

The next step must establish a LevelDB-style baseline and the smallest
attributable production candidate. It must not combine every source-level
difference into one rewrite.

## Source comparison

The current implementations share the high-level lookup order: capture a
snapshot and the current read sources, build an internal lookup key, check the
mutable and immutable memtables, visit overlapping table files, seek the index
and data blocks, and return the first visible value or deletion.

The important differences are:

| Area | Modern LevelDB | Pinned Google LevelDB | Decision for this work |
|---|---|---|---|
| File candidates | Materializes one vector containing sorted level-0 candidates and every deeper-level candidate before opening a table | Materializes level-0 overlap only, then visits deeper levels lazily and stops immediately | Count candidates and searched files first; defer a traversal change |
| Data-block construction | Scans every decoded entry, reconstructs every key, validates every restart, and checks strict comparator order before caching the block | Performs constant-time restart-array bounds setup; entry structure is checked as the iterator visits it, and whole-block key order is not checked | First production candidate removes semantic key-order rescanning while retaining structural safety |
| Internal-key comparison | Validates both byte views and gives malformed keys a deterministic total order on every comparison | Assumes both operands contain an eight-byte trailer | Keep the safe comparator in the first candidate |
| Random-access files | Always reads through `pread` into caller-owned storage | On 64-bit POSIX, normally maps up to 1,000 read-only files before falling back to `pread` | Measure the reference in both default and forced-`pread` modes |
| Block cache | Uses an STL hash table/list and shared ownership | Uses a custom hash table, intrusive references, and an LRU list | Count hits and misses; do not redesign the cache yet |
| Returned value | Allocates and returns an owning byte vector | Writes into a caller-owned string whose capacity the benchmark reuses | Preserve each public API; use missing reads as context rather than adding an artificial copy |
| Iterator allocation | Constructs Modern block iterators in place | Allocates the reference index and data iterators | Do not assume every structural difference favors the reference |

The reference's default POSIX mode is a material difference in the
65,536-record case. Snappy blocks still need decoded storage, but mapping
avoids the Modern path's `pread` call and copy into the stored-block buffer.
The number of live benchmark tables remains below the reference's default
mapping limit, so the reference normally exercises the mapped path.

## Decision

### Use two reference modes

Google LevelDB is the primary behavioral and performance baseline. RocksDB
may inform opt-in diagnostics, but its row cache, `MultiGet`, file picker,
prefetching, and other broader machinery are not the baseline for this task.

Every baseline report must include:

1. Unmodified Modern LevelDB.
2. The pinned Google LevelDB reference with its default POSIX mapping policy.
3. The same reference forced to use its existing `pread` implementation.

The forced-`pread` control runs in a fresh process and sets the read-only mmap
limit to zero before `Env::Default()` is created. The upstream helper currently
keeps `EnvPosixTestHelper::SetReadOnlyMMapLimit` private, so use a minimal,
benchmark-only access patch against the pinned source. The patch may expose
that existing setter but must not replace or edit either upstream read
implementation. Retain the patch, its hash, the source revision, and the
selected mode with the measurement artifacts. Mark the control unsupported on
non-POSIX platforms rather than silently using another policy.

The difference between the two reference modes isolates the effect of file
access inside one engine. It is not a direct prediction of the gain from
adding mmap to Modern LevelDB.

### Add opt-in attribution, not always-on telemetry

Add read-path diagnostics only to a dedicated profiling build. Compile the
hooks out of normal library and benchmark binaries; do not add a null observer
branch, atomics, virtual dispatch, clock reads, or a public API to every
production `Get`. Attach the diagnostic state only to the foreground
benchmark thread, not to process-global counters that can include background
compaction.

The diagnostic build records raw totals and normalized values for:

- Get decisions from the mutable memtable, immutable memtable, SSTables,
  deletions, and complete misses.
- Level-0 candidates, deeper-level candidates, and table files actually
  searched.
- Table-cache and data-block-cache hits and misses.
- Random-access read calls and requested/returned bytes.
- Stored and decoded block counts and bytes, including decompressions.
- Entries visited by block validation and by the data-block seek.
- Bytes copied into successful public results.

These are counters, not a per-request event log. They run only in the
diagnostic binary, which is not used for throughput admission. Add a
fixed-count diagnostic mode for `readrandom` and `readmissing`: after database
creation, verification, and the existing warmup, reset the cursor and
counters, run exactly 4,194,304 foreground Gets, disable collection, and only
then perform final verification and teardown. Record this operation count
with the output. Do not include Google Benchmark calibration callbacks,
setup, warmup, final verification, or background-thread work in the epoch.

The same build may collect wall-clock duration for these coarse operations:
candidate selection, table-cache lookup, block-cache lookup, random-access
read, checksum/decompression, block construction/validation, data-block seek,
and result copy. Select approximately one Get in 4,096 with a deterministic
pseudorandom schedule whose seed is recorded, so sampling cannot remain
aligned with either corpus size. Read the clock only for a selected Get.
Record the exact sample count and whether a duration is inclusive; never sum
overlapping durations or present them as request percentiles.

Use the uninstrumented binary for all throughput conclusions. Diagnostic
counts, sampled timings, CPU profiles, and system I/O traces explain a result;
they do not replace it.

### Trust semantic invariants without weakening memory safety

Data created through `InternalKey`, `LookupKey`, `TableBuilder`, manifest
decoding, recovery, and compaction has explicit validity boundaries. Unit,
model, differential, fuzz, and crash tests are responsible for proving that
our writer emits ordered table keys. A data-block load need not reconstruct
and compare every key again merely to defend against a violation by those
internal producers.

Persisted bytes still cross an external memory-safety and I/O boundary.
Retain explicit checks for:

- Block handles, file ranges, overflow, complete reads, and read errors.
- Block trailers and checksums, compression type, and decompression failures.
- Restart-count bounds, entry varints and lengths, prefix lengths, restart
  offsets, entry boundaries, and checked size arithmetic.
- Internal-key parsing before code consumes a found key as an internal key.

The first candidate changes only data-block construction. Keep the existing
ordered-validation path for the index block, metaindex block, and general
`Block::Create` callers; these blocks are opened once and are not the measured
cache-pressure cost. Add a structural-only construction path used by
`Table::ReadDataBlock`:

- Traverse the encoded entries once.
- Track the previous reconstructed key length rather than reconstructed key
  bytes.
- Prove every entry and restart point required by the trusted iterator is
  structurally safe.
- Do not allocate validation key buffers.
- Do not call the comparator and do not check whole-block key order.

This preserves the one-time proof required by ADR-0046's trusted decoder and
keeps block iterators status-free. It removes semantic revalidation only from
data-block cache misses. A structurally valid data block whose keys are out of
order may consequently produce an incorrect or missing point read, violate
database-iterator ordering, or cause compaction to make an incorrect keep/drop
decision and persist the result. Such a block violates the LevelDB format
ordering contract. This candidate deliberately trusts `TableBuilder` and the
tested internal write path rather than promising to diagnose a
checksum-consistent semantic violation in externally supplied data.

Keep `Table::Get` and the defensive comparator unchanged so the before/after
measurement isolates data-block validation. Under the retained comparator,
every malformed internal key sorts before the valid lookup target, so a
successful seek position remains parseable even if physical key order is
invalid. Any defense-in-depth change to that invariant belongs in a separate
hardening patch.

### Keep internal-key comparison defensive for now

Do not make `InternalKeyComparator::Compare(ByteView, ByteView)` unchecked in
this candidate. Raw SSTable bytes reach comparison before a separate
internal-key parse in block seek, merging iteration, and compaction
grandparent accounting. Current tests also specify deterministic ordering for
malformed keys. Removing checks only inside the comparator would invalidate
those callers rather than move validation to a coherent boundary.

A later comparator experiment must first establish a typed trusted-key
boundary for every caller, or validate each table iterator position before
exposing it to merging and compaction. It may then add a separate trusted
comparison operation while keeping the general byte-view operation safe.
That work must not be combined with block validation, file access, caching, or
result ownership.

### Do not begin with fully lazy block validation

Exact LevelDB-style lazy entry validation is not the first production
candidate. It would simultaneously:

- Remove the one-time structural scan.
- Replace ADR-0046's trusted decoder with checked decoding on every visited
  entry, including cache hits.
- Add an error state or `Status` result to every block-iterator move.
- Change propagation through table, merging, database, and compaction
  iterators.

ADR-0046 already measured a cache-fit benefit from avoiding repeated checked
decoding after one validated boundary. Combining the reverse change with the
removal of semantic validation would obscure attribution and risk trading a
cache-pressure gain for a cache-fit regression. Evaluate lazy validation only
as a later, separate experiment if structural-only validation leaves enough
miss-path cost to justify it.

## Measurement and admission

### Baseline capture

After the diagnostic-tooling change merges, freeze a clean revision and
capture each case in a fresh process. Preserve binaries with their matching
objects or symbol bundles, exact commands, build metadata, source and patch
hashes, raw samples, and honest worktree provenance.

Measure `readrandom` and `readmissing` at 4,096 and 65,536 records for Modern
LevelDB, default LevelDB, and forced-`pread` LevelDB. Run the fixed-count
diagnostic epoch in separate processes for the Modern cases. Repeat the same
diagnostic captures for the exact structural-validation candidate after its
throughput measurement. Report:

- Wall nanoseconds per Get, process CPU nanoseconds per Get, and Get/s.
- Raw and normalized diagnostic counters.
- Sample counts and sampled stage durations.
- Default versus forced-`pread` reference deltas.

The existing benchmark remains a warm page-cache workload. Do not add cache
purges, direct I/O, asynchronous I/O, or a cold-device claim to this slice.

### Structural-validation candidate

Implement the candidate locally from the frozen baseline. Before measurement:

1. Run the applicable local correctness, sanitizer, model, differential,
   crash, and fuzz checks.
2. Obtain one bounded GPT-5.6 Sol review of the exact candidate.
3. Record the base revision, candidate patch, build metadata, executable
   hashes, and symbol-generation logs.

Keep the candidate uncommitted while running the fixed experiment. Any source
change after review or measurement invalidates the affected patch, binary, and
results and requires rebuilding and repeating those steps.

Use the existing paired design: three rounds, three individual repetitions
per process, a 0.3-second minimum, and reversed baseline/candidate order in
the middle round.

Measure all eight Modern read-family cases:

- `readrandom/{4096,65536}`
- `readmissing/{4096,65536}`
- `scan/{4096,65536}`
- `seek_reuse/{4096,65536}`

The primary case is `modern/readrandom/65536`, where uncached data-block
construction is visible. Require:

- At least 5% higher aggregate median Get/s in the primary case.
- Positive primary improvement in every paired round.
- No aggregate regression above 2% in `modern/readrandom/4096`.
- No other Modern read-family case regresses by more than 5% in aggregate or
  by more than 5% in at least two rounds.
- All pre-measurement local correctness gates above pass.

The implementation evidence must show that validation no longer reconstructs
keys or calls the comparator, while malformed entry/restart structure still
fails before trusted decoding. Unordered but structurally valid block tests
must reflect the intentionally weakened diagnostic contract, and
`TableBuilder` tests must continue to reject invalid or non-increasing
internal keys.

If the candidate passes, commit the exact measured patch and update
[ADR-0022](0022-sstable-block-format.md) and
[ADR-0025](0025-sstable-reader.md), together with the iterator and compaction
contracts in [ADR-0031](0031-iterators.md) and
[ADR-0035](0035-running-compactions.md), to describe the accepted trust
boundary. Then run the final cross-platform CI, sanitizer, fuzz, compatibility,
and changed-code coverage gates against that committed revision before
delivery. If the candidate fails, restore production exactly and record the
result here; do not relax the gate, add favorable rounds, or mix in another
optimization.

### Interpretation

Use the controls only for the questions they answer:

- Default versus forced-`pread` LevelDB estimates the effect of its mapped
  access policy under this warm workload.
- Original versus structural-only Modern LevelDB isolates semantic block
  validation.
- Present versus missing reads gives context for value-result ownership but
  does not exactly price one copy.
- Baseline/candidate counter deltas from identical fixed-count epochs verify
  cache and candidate assumptions.
- Sampled timings and CPU profiles identify remaining candidates but do not
  predict additive speedups.

The residual gap is not evidence for one unspecified "CPU overhead." Any
follow-up must select one mechanism with its own admission gate.

## Deferred work

- Fully lazy checked block decoding and iterator error propagation.
- A trusted internal-key type or unchecked comparison operation.
- Lazy deeper-level file traversal and level-0 candidate storage.
- Cache data structures and ownership.
- A reusable-output or caller-buffer public read API.
- Modern mmap-backed random-access files.
- Cold-device, concurrent, tail-latency, prefetch, and asynchronous-I/O
  workloads.
- Bloom filters, row caches, `MultiGet`, and RocksDB-specific file-picking
  machinery.

## Consequences

- The design PR changes no engine behavior.
- The next implementation is diagnostic and reference-control tooling, not a
  production optimization.
- The first production candidate trusts writer-established key order while
  retaining the structural checks required for memory-safe trusted decoding.
- File-access policy and semantic validation become separately measurable
  instead of being inferred from overlapping CPU samples.
- Additional read-path ideas remain sequential and independently attributable.

## Diagnostic tooling implementation

The follow-up tooling builds two binaries from separate library targets:
`modern_leveldb_performance` links the normal library with diagnostics
compiled out, while `modern_leveldb_read_diagnostics` links a diagnostic copy.
The ordinary runner rejects a diagnostic binary as throughput evidence.

The diagnostic runner executes exactly 4,194,304 foreground Gets after the
existing verification and warmup, and before final verification. Its
`splitmix64-v1` schedule with seed 401 selects exactly 991 Gets for inclusive,
non-additive stage timing. The JSON report validates source decisions,
candidate and cache relationships, complete short-read accounting, block
decoding and restart probes, result bytes, corpus fingerprints, and Release
build provenance.

The pinned LevelDB archive is SHA-256 authenticated and populated without
dependency-provider substitution. A build-owned source copy exposes only the
existing POSIX mmap-limit test setter; explicit source overrides remain
unmodified and report the forced-`pread` control as unavailable. Prior
declarations and provider-populated sources are rejected before use.

Local validation built the normal and diagnostic binaries with Apple Clang and
GCC 16, exercised both 4,096-record read modes and the 65,536-record miss path,
and passed all 546 normal unit tests. The normal performance binary contains no
read-diagnostic symbols. A bounded GPT-5.6 Sol implementation review found no
remaining material issue after the source-provenance, short-read,
missing-range, and restart-probe corrections. The structural-validation
candidate remains unstarted.

## Delivery boundary

The bounded design review narrowed the first candidate to data blocks, made
the iterator and compaction consequences explicit, added an exact diagnostic
epoch, and separated local admission from final committed-revision gates.

Obtain one bounded GPT-5.6 Sol design review before merging this ADR. Merge
the design-only PR before diagnostic tooling, review and merge that tooling
before the local validation experiment, and do not start another production
candidate while either change awaits review.

## References

- [ADR-0022 SSTable block format and current validation contract](0022-sstable-block-format.md)
- [ADR-0025 SSTable reader and explicit iterator errors](0025-sstable-reader.md)
- [ADR-0030 point reads](0030-point-reads.md)
- [ADR-0042 benchmark and profiling foundation](0042-benchmark-profiling-foundation.md)
- [ADR-0046 validated block decoding](0046-validated-block-decoding-experiment.md)
- [ADR-0047 rejected inline-key experiment and retained measurements](0047-inline-iterator-key-experiment.md)
- [Google LevelDB point read](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [Google LevelDB block iterator](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/block.cc)
- [Google LevelDB block reading](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/format.cc)
- [Google LevelDB POSIX random-access files](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/env_posix.cc)
- [Google LevelDB POSIX mmap-limit test helper](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/env_posix_test_helper.h)
- [RocksDB PerfContext and IOStatsContext](https://github.com/facebook/rocksdb/wiki/Perf-Context-and-IO-Stats-Context)
