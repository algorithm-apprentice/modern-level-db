# ADR-0053: Pinned LevelDB Read-Path Parity Baseline

- Status: Implemented

## Status

Completed. Milestones 1-4 merged the intrusive cache, block-iterator,
table/mmap, lazy version visitation, typed read pin, exact seek charging, and
reusable output work. The integrated Milestone 5 audit closed two residual
cost-model gaps and one scan-control implementation deviation. The final
frozen-binary matrix passes every completion gate.

## Context

The original read-performance strategy was to use pinned Google LevelDB as the
baseline, first make Modern LevelDB's read path structurally equivalent, and
only then optimize beyond it.

ADR-0049 instead required each difference to be isolated and admitted
independently. That produced two useful accepted changes:

- Data-block construction stopped rechecking writer-established key order,
  improving the 65,536-record point-read case by about 6%.
- Explicit mmap reads improved cache-pressure read families by roughly
  17% through 24%.

It also produced diminishing-return experiments. Inline key storage and
trusted comparison had plausible local evidence but failed point-read
admission and were restored. Trusted comparison covered more than 91% of
internal comparisons yet improved the primary case by only 1.01%.

The failure is strategic rather than evidentiary. A LevelDB mechanism may
depend on its neighboring mechanisms:

- Direct internal comparison assumes LevelDB-style trusted internal keys.
- Cheap block seeking depends on key-buffer reuse and lazy checked decoding.
- Cache costs depend on intrusive handles rather than shared ownership.
- Lazy file traversal avoids work only when table lookup and seek accounting
  stop immediately with it.
- Caller-buffer result reuse removes allocation only when the API permits it.

Requiring each piece to outperform the current mixed architecture can reject
all the pieces even when their complete composition is the intended baseline.
The project therefore stops treating LevelDB parity as a sequence of optional
micro-optimizations.

This ADR supersedes ADR-0052 before its cross-engine diagnostic tooling was
implemented. Existing throughput, fixed Modern diagnostics, source audit, and
warning-free profiles are sufficient to define the parity work. Additional
attribution tooling would delay the baseline it was meant to explain.

## Parity boundary

The target is the point-read path of Google LevelDB revision
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5`, from public `Get` through
`DBImpl::Get`, `Version::Get`, table cache, `Table::InternalGet`, block cache,
stored-block read, and block seek.

Parity means adopting the same control flow, lazy/eager work placement,
ownership cost model, and cache/iterator mechanisms unless an explicit
exception below preserves Modern's public compatibility or memory safety.
It does not mean copying source text or intentionally reproducing an upstream
bug.

An existing Modern mechanism may satisfy a parity row without being rewritten
only when code audit proves that it performs no additional hot-path work,
retains the same lifetime/control-flow semantics, and is strictly no more
expensive by construction. Stack allocation instead of an otherwise identical
heap object, or decoding directly from a stable mapped view without LevelDB's
unused scratch allocation, need not be made worse merely for instruction-level
identity. Record each such decision in the milestone's ADR updates.

## Current divergence checklist

| Layer | Pinned LevelDB | Current Modern | Required parity work |
|---|---|---|---|
| Public result | Caller-owned reusable `std::string` | New owning `vector<byte>` result | Add a caller-buffer overload; retain the owning API as a convenience wrapper |
| Public DB state | Raw DB object during the call | Copies `shared_ptr<DatabaseState>` per call | Avoid per-call shared ownership when the DB object itself must remain alive |
| Read source capture | Intrusive `Ref`/`Unref` on memtables/version | Copies three `shared_ptr`s | Introduce explicit read pins with LevelDB-equivalent lifetime and release points |
| File visitation | L0 temporary candidates, then deeper levels lazily | Builds one vector containing every level's candidate first | Implement callback/visitor traversal with immediate stop |
| Table cache | Intrusive handles and custom hash/LRU | `unordered_map`, `list`, `shared_ptr` handles | Replace the hot table/block cache mechanism with typed intrusive handles |
| Table point lookup | `InternalGet` creates index/data iterators and propagates iterator status | Stack iterators over blocks proven safe at construction | Move structural decoding errors to iterator status/result and follow the same two-level flow |
| Block creation | Validates only restart-array bounds | Scans every entry for structural validity | Remove eager full-entry scans from table block loads |
| Block seek | Checked `DecodeEntry`, reusable `std::string` key | Trusted decoder after eager validation, reusable `vector<byte>` key | Use lazy checked decoding and LevelDB-equivalent short-key storage/reuse |
| Internal comparison | Direct user-key/trailer comparison | Defensive parse and value-kind validation per comparison | Use direct comparison only behind the lazy decoder's minimum-length boundary |
| Block cache ownership | Intrusive pin/release; mapped uncompressed blocks are not double-cached | Shared ownership; mapped uncompressed bytes become an owning copied block | Match pinning, charge, insertion, and mapped-block caching behavior |
| Stored-block read | One `RandomAccessFile::Read`, direct mapped source when returned | Stable-view probe or repeated `pread`, always owning decoded block | Match the mapped/copied source and ownership decision while retaining complete short-read handling |
| POSIX default | Mmap limit 1,000 on 64-bit POSIX | mmap is opt-in with a 1,000-map/4-GiB budget | Enable mmap by default and match the reference count-based budget under the exclusive live-file ownership contract |
| Read checksums | Enabled by benchmark option | Always enabled | Retain Modern's stronger default; benchmark work remains matched |
| Seek charging | First inconclusive searched file | Precomputed candidate vector and later charge | Charge during lazy visitation at the same decision point |

## Milestone 5 audit corrections

The integrated GPT-5.6 Sol review confirmed every control-flow and lifetime
row, but found two remaining hot-path cost differences. They are required
parity fixes, not new optimization experiments.

### Reserve the exact L0 temporary shape

Pinned `Version::ForEachOverlapping` reserves the complete L0 file count and
stores only file pointers in its overlap vector. Modern currently grows an
unreserved `vector<Candidate>`, where every element also stores the level.

Change the temporary to:

```cpp
std::vector<const Version::File*> level0;
level0.reserve(version.files(0).size());
```

Push only overlapping file slots, sort them by descending file number, and
construct `Candidate{.level = 0, .file = file}` only when invoking the
visitor. Retain the existing full-overlap diagnostic count and immediate stop.
This gives one reserved allocation and the pinned pointer-only element shape.

### Put seek budgets on shared file metadata

Pinned `FileMetaData::allowed_seeks` is initialized when a metadata object is
created, shared by versions that retain that object, and decremented directly
under the DB mutex. Modern's `SeekStatistics` map instead allocates on the
first charged Get, copies a file `shared_ptr`, and later touches a weak version
control block.

Add a mutable signed `std::int64_t allowed_seeks` runtime-only field to
`FileMetadata`. It is not part of the MANIFEST encoding.
`VersionBuilder::Apply` initializes it for every newly created metadata object
to:

```text
max(file_size / 16 KiB, 100)
```

Files carried from a base version retain their shared metadata and remaining
budget. A file re-added by an edit, including a trivial move or compaction
output, receives a new metadata object and a fresh budget, matching pinned
builder behavior.

Remove the budget map and weak/shared recorded-version ownership from
`SeekStatistics`. Under the DB mutex:

1. `Charge` requires that `charge.file` is the exact slot in
   `version.files(charge.level)`, not merely another slot holding the same
   metadata. Lookup and read sampling establish this provenance; add a debug
   assertion without a release-path scan.
2. `Charge` decrements `allowed_seeks` through that borrowed slot.
3. If the post-decrement budget is at most zero, the charged version is
   current, and no seek compaction is recorded for current, retain only raw
   current-version identity, the level, and the borrowed file slot. Budgets
   keep falling below zero, as pinned LevelDB does, so an old-version
   exhaustion can be recorded by a later current-version charge.
4. A new `HasFileToCompact(const Version&)` performs the synchronous
   scheduling check without constructing a `shared_ptr`.
5. `FileToCompact(const Version&)` materializes the existing owning
   `SeekCompaction` only when background compaction starts, after that path has
   already captured the current version owner.
6. `Retain` clears the borrowed record whenever the current version identity
   changes.

The current version owns its immutable file-slot vector while a borrowed
record is usable. Version installation and `Retain` both run under the same DB
mutex, so no raw slot survives the version whose identity guards it.

Add tests proving:

- L0 overlap storage is reserved and pointer-only by implementation audit.
- Shared metadata carries its budget across unchanged versions.
- New metadata from an edit receives a fresh budget.
- An old-version read can drive a shared budget below zero without recording;
  a later current-version charge records it.
- The recorded slot always belongs to the version whose raw identity guards
  it; releasing an older version cannot invalidate later materialization.
- Ordinary charges do not increment file or version shared ownership.
- Synchronous `NeedsCompaction` uses the raw recorded state.
- Background compaction receives the owning file only after scheduling.

Do not run the final performance matrix until this correction implementation
passes the complete hardening gates and a bounded implementation review.

The implementation passed Debug/Release, ASan/UBSan, TSan,
compatibility/model/crash, LLVM fuzz, GCC, profiling contracts, and 100%
changed-code coverage. Optimized inspection shows that `Charge` contains no
calls or allocations and that synchronous Get contains no shared-control-block
operations. The bounded GPT-5.6 Sol implementation review found no actionable
issue.

### Scan-control audit correction

The first frozen matrix at `2247447` passed every primary point-read gate but
failed the 4,096-record scan control: +6.87% mapped and +6.24% copied versus
the pre-parity baseline. Milestone attribution placed the regression in the
block-iterator merge, and paired CPU profiles identified
`Table::Iterator::ValidatePosition` as redundant per-entry work.

That helper reparsed each positioned internal key even though reviewed
ADR-0055 explicitly assigns semantic value-kind parsing to point lookup,
`DbIterator`, and compaction. Remove it from `Seek`, edge positioning, `Next`,
and `Prev`. The block iterator still rejects short keys before trusted
comparison and propagates structural corruption; higher consumers still
reject unknown kinds.

The exact three-round local correction gate reduced the matched scan deltas to
+0.82% mapped and -0.05% copied. Full hardening and bounded GPT-5.6 Sol review
found no safety regression. Merge this correction, refreeze the candidate
binary, and rerun the entire final matrix; do not splice the local gate into
the earlier frozen result.

## Decision

### Treat parity as one required baseline

Every checklist row is required. An implementation milestone can be rejected
for correctness, unsafe lifetime, or incompatibility, but not because its
isolated throughput is neutral or negative before the neighboring parity work
exists.

Do not add candidate-specific CPU profiles, fixed-count counters, or admission
thresholds between milestones. Keep ordinary unit, sanitizer, compatibility,
fuzz, compiler, and coverage gates at every merge.

After the final checklist row is complete, run one end-to-end performance
comparison of the cohesive path. If the gap remains, first identify an
unimplemented or intentionally excepted parity row. Do not return to unrelated
micro-optimization experiments while the checklist is incomplete.

### Add reusable result ownership without breaking the API

Add a public overload whose caller supplies a reusable byte vector:

```cpp
Result<bool> Database::Get(
    ByteView key, std::vector<std::byte>& value,
    const ReadOptions& options = {});
```

`true` means a value was found and written to `value`; `false` means missing or
deleted. The method may reuse `value.capacity()`. Errors leave the vector in a
valid but unspecified content state.

Retain the existing owning overload. It creates one vector, calls the reusable
overload, and returns the existing `optional<vector<byte>>` shape. Existing
callers remain source-compatible. The parity benchmark uses the reusable
overload, matching LevelDB's caller-owned `std::string`; a separate control
reports the convenience-overload allocation tax.

### Adopt intrusive read pins and caches

Replace per-Get `shared_ptr` copies for the mutable memtable, immutable
memtable, current version, table-cache entries, and block-cache entries with
explicit typed pins whose increments/releases match LevelDB's read lifetime.

The implementation remains type-safe and RAII:

- No `void*` public cache API.
- A move-only typed handle releases one intrusive reference.
- Deleters and value destruction run outside shard locks.
- Erase/replacement keeps pinned entries alive.
- The cache uses the same 16-shard hash table, LRU/in-use lists, capacity,
  charge, and eviction decisions as the pinned reference.

This supersedes ADR-0014's deliberate standard-container/shared-pointer
deviation for the read caches. It does not replace unrelated public
`shared_ptr` ownership unless it appears on the measured read path.

### Adopt lazy checked block decoding

Table block construction validates only the restart-array region needed to
create an iterator, as the pinned reference does. Iterator moves decode entry
headers, lengths, prefix shares, and boundaries before using them.

The iterator stores an error and every table/merging/compaction move propagates
it through the repository's existing typed `Status` surfaces. A malformed
entry never reaches unchecked memory access or comparison.

Use a reusable short-key representation equivalent to LevelDB's
`std::string` behavior. Do not preserve eager validation merely to keep the
current trusted decoder, and do not separately performance-vote the key
representation.

### Use direct comparison only after a checked boundary

The general `InternalKeyComparator::Compare(ByteView, ByteView)` remains safe
for arbitrary direct callers and malformed-input tests.

The table block iterator uses a private direct comparator after lazy decoding
has established that both operands contain an eight-byte trailer. This keeps
Modern's memory-safety exception without repeating value-kind parsing on every
valid comparison.

Unsupported value kinds remain `Corruption` when an encountered entry is
parsed for point lookup, public iteration, or compaction. Checksum-consistent
semantic mutation of a live database file is outside the supported file
ownership contract.

### Match table and file ownership

Follow LevelDB's `Table::InternalGet` sequence:

1. Seek the index iterator.
2. Apply the filter if configured.
3. Resolve the data block through the block cache.
4. Seek the data iterator.
5. Parse and return the matching user key.
6. Propagate data then index iterator status.

Compressed decoded blocks are owning and cacheable. An uncompressed block
borrowed from a mapped table file is not inserted into the block cache, so it
cannot outlive its file mapping. Copied uncompressed blocks remain owning and
cacheable.

Retain Modern's exact footer, handle-range, non-overlap, checksum, compression,
and complete-short-read checks where they are one-time or I/O-boundary work.
They are explicit safety exceptions, not reasons to keep a repeated hot-path
scan.

### Make mmap the supported POSIX default

While a database is open, its directory is single-process-owned and external
modification of its files is unsupported. Under this contract, a `SIGBUS`
caused by live external truncation is a contract violation rather than a
normal recoverable input.

On supported 64-bit POSIX builds, default immutable table reads to the mapped
path. Match the pinned reference's process-wide limit of 1,000 read-only
mappings; remove the additional 4-GiB byte budget from the default parity path.
Retain fallback to `pread` when mapping is unavailable, the file size is not
exact, or the mapping-count budget is exhausted. Non-POSIX and unsupported
address-width builds continue to use their available backend.

Expose an explicit `pread` opt-out/control so applications with stricter
failure-isolation requirements can retain copied reads. Update ADR-0050 and
public documentation with the clarified ownership contract.

## Sequential implementation milestones

The parity program uses sequential PRs and no parallel development.

### Milestone 1: intrusive cache foundation

- Replace `ShardedLruCache` internals and handles with the pinned LevelDB
  ownership and eviction model.
- Preserve typed values, RAII, destruction outside locks, and existing cache
  call sites.
- Update ADR-0014 and cache/table-cache tests.

### Milestone 2: block and iterator parity

- Replace eager entry scans with lazy checked decoding and iterator error
  state.
- Move to the LevelDB-equivalent reusable key representation.
- Add the checked trusted-comparison boundary.
- Propagate failures through table, merge, database, and compaction iterators.
- Update ADR-0022, ADR-0025, ADR-0031, and ADR-0035.

### Milestone 3: table and mapped-block parity

- Match `InternalGet`, block-cache pinning, mapped-uncompressed borrowing, and
  copied/compressed ownership.
- Make mmap the supported 64-bit POSIX default with explicit copied-read
  control.
- Retain the explicit safety exceptions above.
- Update ADR-0011, ADR-0025, and ADR-0050.

### Milestone 4: version, DB lifetime, and output parity

- Replace eager all-level candidate materialization with LevelDB's lazy
  visitation and immediate stop.
- Match seek charging.
- Replace per-Get source `shared_ptr` copies with typed read pins.
- Add the reusable result overload and make the parity benchmark use it.
- Update ADR-0030 and public API documentation.

### Milestone 5: integrated baseline verification

- Audit every checklist row against the pinned source.
- Run all correctness and hardening gates.
- Obtain one bounded GPT-5.6 Sol review of the complete path.
- Freeze exact binaries and run the final performance matrix.

Milestones may contain several commits but each PR must leave the repository
correct and complete for its stated layer. Do not start a later milestone
until the preceding PR is merged and reviewed.

## Validation

Every milestone runs its smallest complete correctness surface, then the
repository's applicable:

- Native unit and public API tests.
- ASan/UBSan and TSan.
- LevelDB compatibility/model/crash tests.
- Format fuzz smoke.
- AppleClang and GCC warning-clean builds.
- Changed-code coverage.

Add targeted lifetime tests for:

- Read pins surviving version/memtable replacement.
- Cache handles surviving erase, replacement, and eviction.
- Borrowed mapped blocks never entering a cache that can outlive the table.
- Iterator corruption propagating through table, merge, DB, and compaction.
- Reusable output capacity across found, missing, deletion, and error cases.
- Exclusive directory ownership and copied-read opt-out behavior.

## Final performance completion gate

Only the completed Milestone-5 path is measured.

Use three paired rounds, three individual repetitions, and matched mapped and
copied access modes for `readrandom` and `readmissing` at 4,096 and 65,536
records. The 65,536-record cases are primary; 4,096 records are hot-set/fixed
cost controls. Retain scan and seek-reuse as regression controls.

Parity is complete when:

- Modern's reusable-output point reads are within 5% of pinned LevelDB in the
  aggregate median for every primary matched-access case.
- No primary round is more than 10% slower.
- The owning convenience overload's additional allocation/copy cost is
  reported separately rather than hidden.
- No scan or seek-reuse control regresses by more than 5% from the pre-parity
  Modern baseline.

If a point-read case remains outside 5%, the parity audit resumes against the
checklist. The result does not justify unrelated speculative optimization.

## Final frozen result

The final candidate is merge revision
`3799c06ca15db9e36b0f2632078d633d93006171`. The pre-parity Modern control is
`2583eec06cb087ae561c6a478baf94493654d14f`, immediately before the first
implementation milestone.

The clean detached worktrees produced these binaries:

```text
candidate SHA-256:
9d69e8c97b1ce37733e4cf3e4b7c816f03237b9923e1b95bbb40492c8fd3f8a2

pre-parity baseline SHA-256:
5ffc96778e989fbe828ef19b7b753d666f770db85d9a59bef5c3cdc37dc0553b
```

The matrix used three paired rounds, three Google Benchmark repetitions per
fresh process, alternating pair order, a 0.2-second minimum time, and fresh
database paths. Mapped point reads compare both engines' default mmap modes.
Copied point reads use Modern `--modern-file-access pread` and LevelDB
`--reference-file-access pread`.

### Primary point-read gate

Positive deltas mean Modern is slower.

| Access | Workload | Modern ns/op | LevelDB ns/op | Aggregate delta | Three round deltas | Result |
|---|---|---:|---:|---:|---|---|
| mapped | readrandom/65536 | 1001.90 | 989.87 | +1.22% | +1.55%, +0.91%, +1.71% | pass |
| mapped | readmissing/65536 | 967.02 | 979.83 | -1.31% | -1.78%, +2.65%, -2.20% | pass |
| copied | readrandom/65536 | 1265.89 | 1224.04 | +3.42% | +3.61%, +3.70%, +2.85% | pass |
| copied | readmissing/65536 | 1226.21 | 1195.35 | +2.58% | +2.32%, +4.48%, +2.17% | pass |

Every primary aggregate is within 5%, and no primary round is more than 10%
slower. The 4,096-record fixed-cost controls remain slower by 7.94% through
12.55%; ADR-0053 deliberately classifies them as controls rather than primary
completion gates.

### Scan and seek-reuse controls

Each aggregate median compares the final candidate with the frozen pre-parity
Modern binary in the same access mode.

| Access | Workload | Records | Final ns/item | Baseline ns/item | Delta | Result |
|---|---|---:|---:|---:|---:|---|
| mapped | scan | 4096 | 46.78 | 46.33 | +0.97% | pass |
| mapped | scan | 65536 | 79.30 | 104.46 | -24.09% | pass |
| mapped | seek_reuse | 4096 | 2760.79 | 2922.25 | -5.53% | pass |
| mapped | seek_reuse | 65536 | 4106.58 | 4691.95 | -12.48% | pass |
| copied | scan | 4096 | 46.90 | 46.28 | +1.33% | pass |
| copied | scan | 65536 | 104.69 | 134.33 | -22.07% | pass |
| copied | seek_reuse | 4096 | 2753.22 | 2894.91 | -4.89% | pass |
| copied | seek_reuse | 65536 | 4800.70 | 4978.60 | -3.57% | pass |

No scan or seek-reuse aggregate regresses by more than 5%.

### Owning-result control

The owning convenience overload is reported separately from the reusable
parity path:

| Access | Workload | Records | Owning tax |
|---|---|---:|---:|
| mapped | readrandom | 4096 | +6.21% |
| mapped | readrandom | 65536 | +2.56% |
| copied | readrandom | 4096 | +5.81% |
| copied | readrandom | 65536 | +1.46% |

Missing-key controls show no stable owning tax because neither result shape
allocates a returned value; their paired deltas range around measurement
noise and are not interpreted as speedups.

The primary point-read gate, scan/seek control gate, and separate owning-result
reporting requirement all pass. Pinned LevelDB read-path parity is therefore
complete. Further read-path work may now be proposed as post-parity
optimization rather than unfinished baseline implementation.

## Non-goals

- RocksDB-specific row cache, `MultiGet`, prefetch, fractional cascading, or
  cache policies.
- Persisted-format changes.
- Cold-device, direct-I/O, tail-latency, or concurrent-saturation claims.
- Removing typed errors or bounds checks needed for memory safety.
- Optimizing beyond the pinned LevelDB baseline before parity is complete.

## Consequences

- Several previously accepted implementation choices are intentionally
  superseded because they added hot-path ownership or eager work relative to
  the chosen baseline.
- Intermediate milestones may be neutral or slower until the complete path is
  assembled; that is not a reason to revert a correct parity row.
- The final comparison becomes meaningful: remaining differences are explicit
  safety/public-API exceptions rather than a mixture of unfinished baseline
  mechanisms.
- Further optimization begins only after this ADR's checklist and completion
  gate are satisfied.

## Delivery boundary

Merge this design-only PR first. Deliver each milestone through one sequential
PR, waiting for review and merge before starting the next. Do not add more
read-path attribution infrastructure or isolated optimization ADRs until the
integrated baseline is complete.

## References

- [ADR-0014 typed sharded LRU cache](0014-sharded-lru-cache.md)
- [ADR-0022 SSTable block format](0022-sstable-block-format.md)
- [ADR-0025 SSTable reader](0025-sstable-reader.md)
- [ADR-0030 point reads](0030-point-reads.md)
- [ADR-0031 iterators](0031-iterators.md)
- [ADR-0049 LevelDB-style point-read baseline](0049-leveldb-style-point-read-baseline.md)
- [ADR-0050 opt-in POSIX mmap reads](0050-posix-mmap-table-reads.md)
- [ADR-0051 trusted comparison outcome](0051-trusted-internal-key-comparison.md)
- [ADR-0052 superseded gap decomposition](0052-cross-engine-read-gap-decomposition.md)
- [ADR-0058 post-parity 4 KiB fixed-cost alignment](0058-4k-hot-set-fixed-cost-alignment.md)
- [Pinned LevelDB `DBImpl::Get`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [Pinned LevelDB `Version::Get`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [Pinned LevelDB table lookup](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/table.cc)
- [Pinned LevelDB block iterator](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/block.cc)
- [Pinned LevelDB cache](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/cache.cc)
