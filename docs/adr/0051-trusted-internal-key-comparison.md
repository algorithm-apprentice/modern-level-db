# ADR-0051: Trusted Internal-Key Comparison

## Status

Accepted design. Merge this ADR before the separate local implementation and
performance experiment.

## Context

ADR-0049 retained `InternalKeyComparator::Compare(ByteView, ByteView)` as a
defensive operation until every hot caller had an explicit validity boundary.
ADR-0050 removed the largest file-access difference. The remaining mmap read
gap is now concentrated in block seek, key reconstruction, comparison,
decompression, and caching.

Clean mmap-enabled CPU captures from `6b99acb` produced no symbol-generation
warnings:

| `readrandom` case | Samples | Internal comparator inclusive | Internal comparator self | `TryDecodeInternalKey` self |
|---|---:|---:|---:|---:|
| 4,096 records | 6,987 | 34.66% | 6.54% | 3.23% |
| 65,536 records | 6,990 | 20.66% | 2.98% | 1.57% |

Inclusive sample weights overlap and are not additive speedup predictions.
The fixed diagnostic epoch reports about 23 internal-key comparisons per Get
in both 65,536-record point-read cases. Every defensive comparison currently:

1. Checks both lengths.
2. Decodes both trailers.
3. Validates both value kinds.
4. Then performs the user-key and descending-trailer comparison.

That contract is appropriate for arbitrary bytes. It is redundant when types,
construction, recovery, or a preceding table boundary have already
established the minimum representation needed by comparison.

## Current boundaries and callers

The retained profiles permit a smaller first experiment. Comparator samples
with an explicit `Block::Iterator::Seek` ancestor account for 31.32 percentage
points of the 34.66% cache-fit comparator-inclusive weight and 18.40 of 20.66
percentage points under cache pressure: about 90% in both cases.

The audit classifies every production internal-key comparison, but only the
table block-seek rows enter this experiment:

| Caller | Operands | Boundary | Decision |
|---|---|---|---|
| Index-block seek | Index key and constructed seek target | Table open parses every index key before the block is published | Trusted after open |
| Data-block seek | Data key and constructed seek target | Writer order is already trusted; add minimum encoded length before publication | Trusted after load |
| `InternalKey` overloads, version building, compaction picking | Owning `InternalKey` objects | Typed and valid, but not in the measured point-read block-seek hotspot | Defensive in this experiment |
| Memtable skip-list comparator and direct memtable seeks | Internally encoded entries but arbitrary internal `Seek` byte views | No new API boundary in this slice | Defensive |
| `TableBuilder::Add` ordering and separator/successor postconditions | Parsed/current keys | Write path, outside the target | Defensive |
| Deeper-level lookup and `SampleCharge` candidate selection | Typed file keys and parsed/constructed target | Small measured cost, outside the target | Defensive |
| Level and merging iterators | Internal targets/child keys, including direct test/custom iterator surfaces | A broad contract change is unnecessary for point Get | Defensive |
| Compaction grandparent comparison | Input key before `RunCompaction` parses it | Existing malformed-input error contract | Defensive |
| General byte-view API and full block validation | Arbitrary bytes | No table publication boundary | Defensive |
| Metaindex/filter keys | User-format bytes, not internal keys | Bytewise comparator | Unchanged |

The current malformed-key total order remains useful for arbitrary input and
tests. `RecordReadSample → SampleCharge → Candidates` and the
separator/successor postconditions remain defensive. This experiment does not
delete or weaken any of those paths.

## Decision

### Keep two explicit operations

Add:

```cpp
class InternalKeyComparator final : public Comparator {
 public:
  int Compare(ByteView left, ByteView right) const noexcept override;

  int CompareTrusted(ByteView left, ByteView right) const noexcept;
  int Compare(const InternalKey& left, const InternalKey& right) const noexcept;
};
```

`Compare` retains the exact malformed-key total order and current tests.

`CompareTrusted` requires both views to hold at least the eight-byte internal
trailer. It:

- Uses debug assertions for that proven precondition.
- Extracts user-key views directly.
- Decodes each fixed64 trailer without value-kind validation.
- Compares user keys and then trailers in descending order.

Keep the user comparator, trailer ordering, noexcept behavior, and equality
result identical for every valid internal key. The existing `InternalKey`
overload remains defensive in this first experiment.

Unknown value kinds are memory-safe under trusted comparison but are not a
supported semantic value. Data-block value-kind validity joins key order as a
writer-established invariant. Iteration and compaction still return
`Corruption` when they parse an encountered unsupported kind. Point lookup
must parse its found entry explicitly and return `Corruption`; it must no
longer use `.value()` on an unchecked result. A malformed external data key
that is never encountered is not guaranteed to be diagnosed.

### Pass comparison into each iterator

Block iterators depend on the generic `Comparator` interface. Add an internal
`TrustedInternalKeyComparator` adapter that forwards `Compare` to
`InternalKeyComparator::CompareTrusted`. Separator/successor operations may
forward to the parent and are not used by block iteration.

Remove the retained comparator pointer from `Block`. Construction still
receives a comparator when full key-order validation needs it, but immutable
cached blocks retain only bytes and structural layout. Construct
`Block::Iterator` with the comparator it uses:

```cpp
Block::Iterator(const Block& block, const Comparator& comparator);
```

Generic block callers and tests pass the existing defensive/user comparator.
`Table::Get` and `Table::Iterator` pass the trusted adapter only after their
table boundaries. A cached block can therefore outlive its table or database
without retaining a dangling adapter or user-comparator pointer.

`DatabaseEngine` owns one adapter beside its `InternalKeyComparator` and
before `TableCache`. `TableOptions` carries a non-owning pointer that must
outlive the table and its table iterators, but not block-cache entries. Direct
table callers may omit it and retain defensive block seeks.

### Establish table boundaries before using the adapter

Add a data-block factory that combines the ADR-0049 structural/order trust
with a minimum key length:

```cpp
Block::CreateWithTrustedInternalKeyOrder(contents);
```

It retains every structural, restart, entry-boundary, prefix-length, and
checked-arithmetic validation. It additionally rejects any reconstructed key
whose length is below `InternalKeyTrailerSize`. It does not reconstruct full
keys, validate value kinds, or compare key order.

Index blocks keep full construction-time order validation with the defensive
comparator. `ValidateIndex` parses every index key while it already walks every
value and block handle. No comparison occurs during that forward validation
walk. Only after successful index-key parsing is the table published;
subsequent index seeks construct iterators with the trusted adapter.

Data blocks reject reconstructed key lengths below `InternalKeyTrailerSize`
before publication. They do not validate value kinds or compare key order.

`Table::Iterator::Seek` accepts a raw internal target, so it parses that target
once before constructing any trusted seek. `Table::Get` uses a valid
`LookupKey`. Direct `Block::Iterator` users select their comparator explicitly
and therefore cannot accidentally enter the trusted operation.

### Limit the first rollout to table block seek

Only `Table::Get` and `Table::Iterator` use the trusted adapter, for their
validated index and data blocks. Memtables, file candidate selection,
level/merging iterators, compaction, typed metadata, builder ordering,
`SampleCharge`, and separator/successor logic retain the defensive operation.
Do not broaden the experiment if its measured gain misses admission.

Do not change:

- The user comparator.
- Public `Comparator` or database APIs.
- Persisted bytes.
- The general defensive byte-view comparison.
- Checksum, decompression, block structural validation, or mmap behavior.
- Key reconstruction storage, cache implementation, file candidate traversal,
  or result ownership.

### Attribute trusted and defensive work

Retain the total internal-comparison counter and add trusted and defensive
sub-counters, with trusted index/data roles. During each fixed point-read
diagnostic:

- `trusted_index + trusted_data + defensive == total`.
- Candidate total comparisons/Get differ from the frozen baseline by at most
  2%.
- Both trusted index and trusted data counts are nonzero.
- Trusted index/data comparisons account for at least 80% of the candidate
  total, matching the profile-selected boundary without forcing unrelated
  callers into the experiment.
- Every table open either validates all index internal keys or fails before
  the measured epoch.

These are count invariants. CPU profiles remain sampled attribution and do not
replace throughput.

## Validation plan

### Comparator and boundary tests

- Trusted and defensive comparison return the same sign for valid internal
  keys across randomized user keys, sequences, value kinds, and custom user
  comparators.
- The defensive operation retains deterministic ordering for malformed keys.
- Debug builds reject short trusted operands through assertions; Release code
  contains no repeated validity branch.
- Typed `InternalKey`, separator, successor, memtable, candidate, iterator,
  and compaction comparison behavior remains byte-for-byte unchanged.

### Table and iterator tests

- Trusted data-block creation rejects every structurally malformed block and
  every reconstructed key shorter than eight bytes.
- Structurally valid, ordered writer output remains readable by lookup,
  forward/reverse iteration, merging, and compaction.
- Table open rejects a malformed index internal key before any trusted index
  seek.
- `Table::Iterator::Seek` rejects a short or invalid target before trusted
  comparison; direct block iteration with the defensive comparator remains
  safe for arbitrary targets.
- Point lookup returns `Corruption` for an encountered unsupported data value
  kind instead of assuming parse success.
- Existing memtable, database iterator, merging, compaction, and `SampleCharge`
  malformed-input tests remain valid because those paths stay defensive.
- A cached block can outlive its table and database because it retains no
  comparator pointer.

Use existing unit/model, ASan/UBSan, TSan, LevelDB compatibility, fuzz,
platform, and changed-code coverage gates. Inspect optimized code to confirm
the trusted operation does not call `TryDecodeInternalKey`.

## Measurement and admission

Merge this design, then freeze a clean baseline containing the accepted mmap
implementation. Keep the exact candidate local and uncommitted through
correctness review and measurement.

Use three paired rounds, three individual repetitions, a 0.3-second minimum,
and the existing alternating process order for all eight Modern read-family
cases.

The primary case is cache-fit `modern/readrandom/4096`. Require:

- At least 3% higher aggregate median Get/s.
- Positive primary gain in every paired round.
- No aggregate regression above 2% in `modern/readrandom/65536`.
- No other case regresses by more than 5% in aggregate or by more than 5% in
  at least two rounds.
- The diagnostic comparison-count invariants above hold.
- All correctness and delivery gates pass.

The 3% threshold is intentionally lower than the 5-10% gates used for larger
or public-contract changes. This candidate adds a private adapter, moves only
the table block-seek boundary, and removes comparator retention from immutable
blocks; it adds no allocation, persisted state, configuration, or public API.
The cache-fit profile directly assigns 3.23% self sample weight to repeated key
decoding, so a stable 3% end-to-end gain is material.

Use ADR-0050's exact instability trigger and fixed-work confirmation protocol:
round spread above 10%, 262,144 iterations, five repetitions, candidate-first
then baseline-first pairs, pooled ten-sample medians, and the same
case-specific pass rules. For the primary case, replace the fixed aggregate
threshold with 3% while still requiring both pair gains to be positive.

If the candidate passes, capture a warning-free cache-fit CPU profile, commit
the exact measured patch, and update ADR-0012, ADR-0022, ADR-0025, ADR-0031,
and this ADR. If it fails, restore production and record the result without
combining another optimization.

## Deferred work

- Inline/reusable key reconstruction, including revisiting ADR-0047 for scans.
- Lazy file-candidate traversal.
- Cache data structures and ownership.
- Reusable-output or caller-buffer APIs.
- Lazy checked block decoding.
- Stronger validation of externally supplied tables beyond the trusted writer
  contract.

## Consequences

- Arbitrary byte comparison remains safe and deterministic.
- Validated index/data block seeks can avoid repeated length/kind validation.
- Data blocks gain a minimum-length safety boundary without restoring the
  full key reconstruction removed by ADR-0049.
- Index keys become explicitly validated once at table open.
- Cached blocks no longer retain comparator pointers.
- Unsupported data value kinds remain corruption when encountered, but the
  reader no longer promises to discover every semantic violation in an
  externally supplied writer-invalid table.

## Delivery boundary

The bounded design review narrowed the candidate from all proven internal
callers to table block seeks, added one-time target/index boundaries, and
removed comparator retention from cached blocks.

Obtain one bounded GPT-5.6 Sol design review and merge this design-only PR
before implementation. Review the exact candidate before measurement and
commit it only after admission. Keep key storage, cache, candidate traversal,
and result ownership separate.

## References

- [ADR-0012 internal-key format and malformed ordering](0012-internal-key-format.md)
- [ADR-0022 block validation and trusted data order](0022-sstable-block-format.md)
- [ADR-0025 table reader](0025-sstable-reader.md)
- [ADR-0031 internal iterators](0031-iterators.md)
- [ADR-0047 inline-key experiment](0047-inline-iterator-key-experiment.md)
- [ADR-0049 point-read boundaries and diagnostics](0049-leveldb-style-point-read-baseline.md)
- [ADR-0050 mmap read outcome](0050-posix-mmap-table-reads.md)
- [Pinned LevelDB internal comparator](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/dbformat.cc)
