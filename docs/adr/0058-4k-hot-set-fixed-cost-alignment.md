# ADR-0058: 4 KiB Hot-Set Fixed-Cost Alignment

## Status

Accepted outcome. The amended one-sided gate treats pinned LevelDB as a
no-more-than-5%-slower baseline. A fresh comparator-only candidate passed
every point-read and regression-control condition, so the stage-2 decoder was
not implemented in production.

## Context

[ADR-0053](0053-leveldb-read-path-parity.md) completed the pinned Google
LevelDB point-read baseline. Its frozen 65,536-record primary cases all pass:
Modern ranges from 1.31% faster to 3.42% slower than pinned LevelDB across
matched default-mmap and forced-pread modes.

The 4,096-record fixed-cost controls remain slower:

| Access | Workload | Modern versus LevelDB |
|---|---|---:|
| default mmap | `readrandom/4096` | +12.55% |
| default mmap | `readmissing/4096` | +10.24% |
| forced pread | `readrandom/4096` | +12.27% |
| forced pread | `readmissing/4096` | +7.94% |

The similar gap for mapped and copied reads rules out file access as the
primary cause. The gap on missing reads rules out successful-value output
copying and ownership. The 65,536-record results show that cache-pressure and
I/O work amortize or offset the remaining per-Get CPU cost.

This is post-parity optimization. It does not reopen the ADR-0053 checklist or
permit weakening Modern's explicit corruption-safety exceptions.

## Evidence

### Matched CPU capture

The frozen ADR-0053 candidate
`3799c06ca15db9e36b0f2632078d633d93006171` and its pinned LevelDB adapter
were captured on `readrandom/4096` from the same profiling executable:

| Engine | Wall ns/op | Process CPU ns/op | Samples |
|---|---:|---:|---:|
| Modern | 450.87 | 450.24 | 7,037 |
| pinned LevelDB | 421.51 | 421.47 | 6,984 |

Profiled timings are attribution evidence only, not admission measurements.

Both captures place most inclusive time in block seek: 63.32% for Modern and
65.82% for LevelDB. Mutex self time is close, and Modern's synchronous Get has
no shared-control-block operations.

Modern's self samples expose two concrete differences:

- `BytewiseComparatorImpl::Compare` compiles to a scalar byte-at-a-time loop.
  Its generic libc++ comparison layers appear separately in the profile:
  `lexicographical_compare_three_way` 11.84%, `BytewiseComparatorImpl` 10.02%,
  and `compare_three_way` 9.54%.
- `Block::Iterator::ParseNextEntry` has a 176-byte stack frame and calls an
  out-of-line `DecodeEntry`. `ParseNextEntry`, `DecodeEntry`, and
  `DecodeVarint32` account for 6.31%, 3.72%, and 2.13% self time.

Pinned LevelDB instead:

- Implements `Slice::compare` with `memcmp` followed by length comparison.
- Uses an inline pointer/limit `DecodeEntry`.
- Uses a one-byte `GetVarint32Ptr` fast path and calls a loop only for a
  multibyte varint.
- Uses an internal boolean parse result and exposes `Status` at the iterator
  boundary.

### Existing experiments do not answer this question

ADR-0051's rejected candidate changed only trusted internal-key parsing. It
left the user comparator's scalar byte loop in place, improved
`readrandom/4096` by only 1.01%, and had two negative rounds.

ADR-0046's earlier validated-decoder experiment moved checks to block
construction. ADR-0055 deliberately superseded that work placement with lazy
checked decoding so corruption remains recoverable. This ADR may reduce the
successful lazy-decoder machinery, but it may not restore eager validation or
an unchecked iterator.

## Goal

Make all four 4,096-record point-read controls no more than 5% slower than
pinned LevelDB without regressing the completed 65,536-record parity result,
scan/seek controls, or corruption safety. Faster Modern results are valid and
do not fail alignment.

## Performance-baseline amendment

Pinned LevelDB is the implementation and performance baseline, not a
performance ceiling. The original wording used an absolute ±5% interval.
That is suitable for measuring numerical closeness, but it is wrong for a
post-parity optimization: it rejects improvements solely because Modern is
more than 5% faster.

Replace every point-read aggregate admission check with:

```text
Modern / LevelDB - 1 <= +5%
```

and retain:

```text
every individual round delta <= +10%
```

Negative deltas of any magnitude pass. Scan/seek controls likewise continue
to fail only when the candidate is more than 5% slower than the frozen Modern
baseline; improvements pass.

This amendment is prospective. The prior local comparator and combined
measurements do not decide acceptance. After this amendment is reviewed and
merged, rebuild the exact comparator-only candidate from a fresh detached
worktree, freeze new binary/patch/compile-command hashes, and rerun the
complete matrix into a new artifact root. If that fresh stage-1 matrix passes,
stop and do not include the decoder. Only a fresh stage-1 failure permits a
fresh combined build and matrix.

## Scope

This ADR covers two sequential, predesigned stages in the common cached point
read path:

1. Exact LevelDB-style bytewise comparison through `memcmp`.
2. If stage 1 does not meet the complete 4 KiB gate, an inline checked block
   entry decoder with LevelDB-style one-byte varint fast paths and internal
   boolean movement.

It also covers:

- Comparator and block-iterator tests.
- Optimized code-generation inspection.
- The fixed stop/admission matrix.
- Direct documentation updates.

It does not cover:

- Mmap, pread, block/table cache policy, read pins, seek charging, or result
  ownership.
- Public API changes.
- Inline/custom key containers.
- Eager full-block validation.
- Unchecked decoding, speculative overreads, native-endian loads, or removing
  typed iterator errors.
- Persisted-format changes.
- Concurrent throughput or tail-latency claims.

## Stage 1: bytewise comparison

### Match `Slice::compare`

Replace `std::lexicographical_compare_three_way` in
`BytewiseComparatorImpl::Compare` with the pinned shape:

```cpp
const std::size_t size = std::min(left.size(), right.size());
const int order = size == 0 ? 0 : std::memcmp(left.data(), right.data(), size);
if (order < 0) {
  return -1;
}
if (order > 0) {
  return 1;
}
if (left.size() < right.size()) {
  return -1;
}
if (left.size() > right.size()) {
  return 1;
}
return 0;
```

Do not call `memcmp` for an empty comparison, because a default empty
`ByteView` need not carry nonnull storage. Normalize the result to -1, 0, or
1, preserving the existing comparator contract rather than exposing the
implementation-defined magnitude of `memcmp`.

This is valid for `std::byte` storage: bytewise keys are contiguous object
representations, and `memcmp` compares them as unsigned characters.

`FindShortestSeparator`, `FindShortSuccessor`, and the comparator name remain
unchanged.

### Comparator verification

Retain and run the existing exhaustive 256-by-256 single-byte ordering test,
empty/default storage cases, embedded nulls, prefixes, unaligned and
overlapping views, and randomized scalar-oracle comparisons.

Add no private-implementation test. Optimized inspection must prove:

- AppleClang emits a `memcmp` call/intrinsic for dynamic nonempty ranges.
- GCC emits `memcmp` or its builtin equivalent.
- The previous scalar dynamic-length byte loop is absent.

### Stage-1 stop condition

After all correctness gates, freeze a comparator-only executable from the
merged design base. Run the complete fixed measurement matrix below.

If every admission condition passes, stop: do not implement stage 2 merely
because it was designed.

If any admission condition fails, retain the stage-1 source only as part of
the predeclared combined candidate and proceed directly to stage 2. This
includes a 65,536-record or scan/seek control failure even if all 4,096-record
cases pass. The comparator-only result is not separately accepted, rejected,
or tuned.

## Stage 2: checked decoder fast path

Stage 2 is implemented only when the stage-1 matrix does not meet the complete
4 KiB gate.

### Preserve the lazy safety boundary

Blocks still validate only the restart-array bounds at construction. Every
encountered entry remains checked before pointer formation, key
reconstruction, comparison, or value exposure.

Retain all current checks:

- At least three input bytes before reading the header fast path.
- At most five bytes per varint32.
- No read past `limit`.
- A representable key/value extent using `std::size_t` arithmetic.
- `shared <= key_.size()`.
- The reconstructed internal key contains its eight-byte trailer in trusted
  mode.
- Restart offsets remain in the entry region and restart keys have
  `shared == 0`.
- Allocation happens before iterator state mutation.

Do not copy pinned LevelDB's 32-bit `non_shared + value_length` overflow risk.
Check the extents as:

```cpp
if (non_shared > remaining ||
    value_length > remaining - non_shared) {
  return nullptr;
}
```

### Use pinned varint control flow

Replace the general five-iteration helper on the common path with:

```cpp
inline const std::byte* DecodeVarint32Ptr(
    const std::byte* input,
    const std::byte* limit,
    std::uint32_t& value) noexcept;

const std::byte* DecodeVarint32Fallback(
    const std::byte* input,
    const std::byte* limit,
    std::uint32_t& value) noexcept;
```

`DecodeVarint32Ptr` consumes one byte directly when its continuation bit is
clear. Only a multibyte value calls the bounded fallback loop. Preserve
LevelDB-compatible nonminimal encodings and fifth-byte excess payload
semantics already covered by ADR-0055.

Use an inline scalar-output `DecodeEntry`:

```cpp
inline const std::byte* DecodeEntry(
    const std::byte* input,
    const std::byte* limit,
    std::uint32_t& shared,
    std::uint32_t& non_shared,
    std::uint32_t& value_length) noexcept;
```

Avoid constructing a `DecodedEntry` containing two spans before the entry has
passed its checks. `ValidateEntries`, restart-key decoding, and
`ParseNextEntry` share this checked helper. Form key/value views only after
the returned key-delta pointer and lengths are proven valid.

The initial implementation uses normal portable `inline`. Do not add
compiler-specific force-inline attributes without an ADR amendment and
evidence from both AppleClang and GCC.

### Keep typed errors at the iterator boundary

Match pinned `ParseNextKey` internally while preserving Modern's public
contract:

- `ParseNextEntry()` returns true exactly when it positions at a valid entry.
  End-of-block invalidates without setting `error_` and returns false.
  Malformed input invalidates, stores the existing typed `Error`, and returns
  false.
- `RestartKey` and `SeekToRestartPoint` return true on success. Their false
  result always stores corruption.
- A private `MoveStatus()` returns success when `error_` is empty and returns
  its typed corruption otherwise. Public `Seek`, `Next`, `Prev`,
  `SeekToFirst`, and `SeekToLast` call it once when a private boolean move
  stops.
- `Corruption()` becomes the cold false-returning helper that invalidates and
  stores the error.
- Positioning still clears an earlier error and can recover.

Do not construct `Error` or `Status` objects on each successful inner-loop
iteration. Error construction remains cold and explicit.

`ParseNextEntry` retains the current strong allocation ordering: decode and
check extents, compute the reconstructed key size, reserve key capacity, and
only then change `current_`, `key_`, `value_`, or `next_`. After reserve,
resize/append must fit the established capacity. An allocation exception
therefore leaves the prior iterator position and error state unchanged.

Diagnostics continue to count only successfully decoded restart/data/index
entries.

### Decoder code-generation gate

In optimized normal and diagnostic builds:

- `ParseNextEntry` and restart-key decoding do not call an out-of-line
  `DecodeEntry`.
- One-byte varints do not call the fallback loop.
- The successful path performs no `Error` construction.
- No bounds check from the reviewed list disappears.

## Fixed measurement protocol

### Frozen inputs

Use:

- Modern baseline:
  `3799c06ca15db9e36b0f2632078d633d93006171`.
- Pinned LevelDB:
  `7ee830d02b623e8ffe0b95d59a74db1e58da04c5`.
- The existing profiling preset and deterministic corpus.
- A clean, detached baseline worktree/build directory.
- A separate candidate worktree/build directory based on the merged ADR-0058
  design revision. Candidate production changes remain uncommitted until the
  fixed admission decision, matching ADR-0046's experiment discipline.

Record source revisions, dirty state, the exact candidate patch and its
SHA-256, compile commands, and executable SHA-256 hashes. A dirty candidate is
valid only with that complete provenance. Do not change the patch or rebuild a
frozen executable between rounds.

### Pairing

Use three paired rounds and three framework repetitions per fresh process.
Alternate pair order in round two. Use a 0.2-second minimum time and the
runner's validated fresh database lifecycle.

For each stage decision, run:

- Modern versus LevelDB:
  - `readrandom/4096`
  - `readmissing/4096`
  - `readrandom/65536`
  - `readmissing/65536`
  - default mmap and forced pread.
- Candidate versus frozen Modern baseline:
  - `scan/4096`, `scan/65536`
  - `seek_reuse/4096`, `seek_reuse/65536`
  - default mmap and forced pread.

Do not add favorable rounds, adaptive minimum times, or a candidate-specific
workload.

Use the frozen candidate executable for both its Modern and pinned-LevelDB
point-read processes so build environment and dependency provenance match.
Use the frozen baseline executable only for candidate-versus-baseline
scan/seek controls.

The wall-time gate is calculated as follows:

- Each invocation yields three individual `wall_ns_per_item` repetitions.
- A round delta compares the two variants' medians within that round.
- The aggregate delta compares the two variants' medians across all nine
  individual repetitions.
- Delta is `Modern / LevelDB - 1` for point reads and
  `candidate / baseline - 1` for controls.

Process CPU results are recorded as supporting evidence but do not replace
the wall-time admission gate.

### Admission

The final candidate passes only when:

- No 4,096-record Modern/LevelDB aggregate median is more than 5% slower.
- No 4,096-record round is more than 10% slower.
- No existing 65,536-record primary aggregate is more than 5% slower.
- No 65,536-record primary round is more than 10% slower.
- No scan or seek-reuse aggregate regresses by more than 5% from the frozen
  Modern baseline.
- All correctness and hardening gates pass.

If stage 1 meets every condition, it is the final candidate and stage 2 is
skipped.

If stage 1 misses any admission condition, implement stage 2 and rerun the
complete matrix. The combined candidate is accepted only if every condition
passes. If it fails, restore both production changes and record the rejected
outcome; do not cherry-pick whichever individual rows look favorable after
the fact.

## Validation

Before either measurement:

- Full Debug and Release unit/public API suites.
- ASan/UBSan and TSan.
- LevelDB compatibility, model, and crash suites.
- LLVM format/database fuzz smoke.
- AppleClang and GCC warning-clean optimized builds.
- 100% changed-code line/branch coverage.
- Comparator and decoder code-generation inspection.
- Bounded GPT-5.6 Sol implementation review of the exact candidate.

For stage 1, decoder-specific gates are not applicable. If stage 2 is needed,
rerun the complete hardening and implementation-review surface on the
combined candidate before its measurement.

Decoder tests retain:

- One- through five-byte varints.
- Nonminimal and accepted fifth-byte payload encodings.
- Empty/binary keys and values.
- Restart and prefix-compressed entries.
- Forward/backward iteration, seeks, direction changes, and recovery.
- Truncated/unterminated headers, oversized extents, invalid shared prefixes,
  invalid restarts, short internal keys, and lazy corruption propagation
  through table, merging, DB iterator, and compaction layers.

## Outcome

### Excluded pre-amendment measurements

The comparator and combined matrices collected before the one-sided gate
amendment do not decide this outcome. Some earlier local artifacts also reused
a build directory with stale embedded configure provenance. All such
artifacts remain diagnostic only.

After amendment PR #79 merged as
`6399425482be2465cf2ff3361cc06ad24ef55282`, the comparator-only candidate was
rebuilt from a fresh detached worktree. Its report correctly records that
revision and `configure_dirty=true`.

### Frozen artifacts

```text
candidate source patch SHA-256:
35d2b1a240f99a7c1465581c28393c42566828ad06c483e360cd0df313a4741c

candidate executable SHA-256:
e76f2a422517ddf19619be7aa6a6f134768d444efd2f12736ed1e4755df71be2

candidate compile_commands.json SHA-256:
cbb2601bb42c0e32fa29bfd74ce911bc894365fb30371a6969c50eaf1f129585

frozen Modern baseline revision:
3799c06ca15db9e36b0f2632078d633d93006171

frozen baseline executable SHA-256:
9d69e8c97b1ce37733e4cf3e4b7c816f03237b9923e1b95bbb40492c8fd3f8a2
```

The candidate passed the full native, sanitizer, compatibility/model/crash,
LLVM fuzz, GCC, profiling-contract, and 100% changed-code coverage gates.
AppleClang and GCC both emit a `memcmp` call for dynamic nonempty comparisons;
the prior scalar byte loop is absent. Bounded GPT-5.6 Sol implementation
review found no actionable issue.

### Point-read gate

Positive deltas mean Modern is slower. Negative deltas are speedups and pass.

| Access | Workload | Modern ns/op | LevelDB ns/op | Aggregate delta | Three round deltas | Result |
|---|---|---:|---:|---:|---|---|
| mapped | `readrandom/4096` | 414.94 | 401.33 | +3.39% | +4.10%, +1.92%, +3.79% | pass |
| mapped | `readmissing/4096` | 410.11 | 407.25 | +0.70% | +0.76%, -1.37%, +0.70% | pass |
| copied | `readrandom/4096` | 421.10 | 407.24 | +3.40% | -0.33%, +4.00%, +3.63% | pass |
| copied | `readmissing/4096` | 416.30 | 413.84 | +0.59% | +2.01%, -0.09%, +1.33% | pass |
| mapped | `readrandom/65536` | 899.91 | 945.11 | -4.78% | -4.78%, -7.99%, -2.42% | pass |
| mapped | `readmissing/65536` | 897.28 | 968.96 | -7.40% | -4.76%, -5.86%, -11.22% | pass |
| copied | `readrandom/65536` | 1143.41 | 1193.53 | -4.20% | -3.46%, -5.86%, +9.69% | pass |
| copied | `readmissing/65536` | 1086.83 | 1148.19 | -5.34% | -3.21%, -6.65%, -5.34% | pass |

No aggregate is more than 5% slower, and no round is more than 10% slower.

### Regression controls

| Access | Workload | Records | Candidate versus baseline | Result |
|---|---|---:|---:|---|
| mapped | scan | 4096 | -0.20% | pass |
| mapped | scan | 65536 | -0.00% | pass |
| mapped | seek-reuse | 4096 | -7.67% | pass |
| mapped | seek-reuse | 65536 | +2.31% | pass |
| copied | scan | 4096 | -0.83% | pass |
| copied | scan | 65536 | -1.35% | pass |
| copied | seek-reuse | 4096 | -30.69% | pass |
| copied | seek-reuse | 65536 | +0.25% | pass |

No aggregate control regresses by more than 5%.

### Decision

Accept the comparator-only candidate. It closes the 4 KiB hot-set gap without
regressing the completed pressure or scan/seek gates. The conditional checked
decoder stage is skipped exactly as the predeclared stop rule requires.

The pinned reference still has optional external hardware CRC32C disabled.
That separately identified control remains future work and does not alter this
candidate's admission.

## Documentation and delivery

Use sequential pull requests:

1. This design-only ADR.
2. One implementation/outcome PR after the fixed stop condition and final
   admission decision.

Update ADR-0053 only with the final accepted or rejected outcome. Preserve raw
reports and code-generation evidence locally; commit exact aggregate/round
results and binary identities.

## Rejected alternatives

### Optimize mutexes or read pins first

Rejected. Their matched profile cost is small, and ADR-0057 already removed
shared-control-block work from synchronous Get.

### Change result ownership or copying

Rejected. Reusable output is already the parity path, and missing reads show a
similar gap without copying a found value.

### Revisit mmap or cache ownership

Rejected. The gap is similar in mmap and pread modes, while 65,536-record
cache-pressure cases already pass.

### Restore eager validation and unchecked decoding

Rejected. It would reverse ADR-0055's recoverable corruption boundary and
trade memory safety for a benchmark result.

### Add a custom inline key container

Rejected. ADR-0047's isolated inline-key candidate did not meet its point-read
gate. `std::string` already provides the pinned short-string behavior.

### Combine unrelated fixed-cost changes

Rejected. The profile identifies comparator and decoder control flow. Cache,
API, allocator, or metadata changes would obscure the decision and expand the
review surface.

## Consequences

- A simple exact comparator change gets the first opportunity to close the
  hot-set gap.
- The comparator-only candidate passed, so decoder complexity is not added to
  production.
- Every accepted path retains Modern's typed corruption behavior.
- Safe speedups are accepted rather than rejected for exceeding an artificial
  closeness ceiling.
- A passing result makes the 4 KiB controls part of the LevelDB-aligned
  performance envelope; a rejected result leaves the completed ADR-0053
  baseline unchanged.

## References

- [ADR-0046 validated block decoding experiment](0046-validated-block-decoding-experiment.md)
- [ADR-0047 inline iterator key experiment](0047-inline-iterator-key-experiment.md)
- [ADR-0051 trusted internal-key comparison outcome](0051-trusted-internal-key-comparison.md)
- [ADR-0053 completed read-path parity](0053-leveldb-read-path-parity.md)
- [ADR-0055 block-iterator parity](0055-leveldb-block-iterator-parity.md)
- [Pinned LevelDB `Slice::compare`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/slice.h)
- [Pinned LevelDB block decoder](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/block.cc)
