# ADR-0043: Profile-Guided Bytewise Comparison

## Status

Experiment completed; the candidate was rejected by its admission criteria.
The original comparator remains in production.

## Evidence and candidate selection

ADR-0042's infrastructure is merged as PR #39. Its measured-window CPU
captures and separate unprofiled runs identified these first candidates:

| Workload | Observed cost | Candidate and disposition |
|---|---|---|
| Cache-fit random reads, 4,096 records | Bytewise comparator call chain: about 23.6% inclusive CPU sample weight | First: use the proven contiguous-byte comparison primitive |
| Cache-pressure random reads, 65,536 records | `ExtendCrc32c`: about 42.6% self sample weight | Next candidate: evaluate a mature faster CRC implementation separately |
| Cache-pressure scans, 65,536 records | `ExtendCrc32c`: about 49.6% self sample weight | Same CRC candidate; do not mix it into comparator attribution |
| Cache-pressure scans | Cache insertion: about 5.5% inclusive sample weight | Do not redesign the cache ahead of the dominant costs |

Inclusive weights include callees and overlap; they are not added to self
weights or presented as guaranteed speedups. These observations are from
optimized macOS captures, not a universal ranking for every platform.

The current bytewise comparator uses
`std::lexicographical_compare_three_way` over `std::byte`. In the captured
build, its element-comparison/template paths remain material CPU consumers.
Both pinned Google LevelDB and RocksDB v11.8.1 instead compare contiguous
bytes with `memcmp`, then compare lengths if the common prefix is equal.

This first change is chosen for its high observed relevance and small
correctness surface. It adds no dependency, format, hardware-specific code,
configuration knob, or new ownership convention.

## Decision

Change only `BytewiseComparatorImpl::Compare` in `src/base/comparator.cc`:

1. Compute the minimum input length.
2. If it is nonzero, compare exactly that many bytes with `std::memcmp`.
3. Normalize a nonzero result to `-1` or `1`, retaining the current exact
   result range rather than exposing a library-specific difference magnitude.
4. If the common bytes compare equal, order the shorter input first, or return
   zero for equal lengths.

Do not call `memcmp` for zero bytes: empty `ByteView` may have a null data
pointer, and the C function's pointer contract must not be assumed away.

`memcmp` compares object representations as unsigned bytes, which matches
the existing ordering of every `std::byte` value. Embedded NULs, high bytes,
prefixes, aliasing/overlapping read-only views, and unaligned views remain
valid. No input is modified and no allocation is added.

Keep the comparator name `leveldb.BytewiseComparator`, `noexcept`, shortest
separator/successor behavior, and all public ownership contracts unchanged.
The stable name is justified by identical ordering semantics, not by an
assumption that callers tolerate a new ordering.

Do not remove block validation, change checksums, add caching, or alter
snapshot/compaction behavior in this implementation.

## Verification and admission

This is a behavior-preserving optimization, so keep focused tests green
before and after the change rather than manufacturing a failing behavior test.
Add an independent scalar-byte oracle covering all 256 one-byte values,
empty/null views, unequal/equal prefixes, embedded zeros, unaligned and
overlapping subviews, and deterministic varied-length inputs. Assert exact
`-1/0/1` results as well as ordering.

Run the relevant comparator/internal-key/block/iterator tests and the existing
unit, sanitizer, compatibility, crash, fuzz, and changed-code coverage gates.
Format and snapshot semantics must not change.

Freeze a clean baseline executable from the merged design revision, its build
metadata, executable digest, and unprofiled results before implementation.
Compare the candidate and frozen baseline on the same host with ADR-0042's
unchanged workloads/options. Alternate baseline/candidate process order and
retain individual repetitions, rather than comparing one fresh result with
an older number from another workload or machine.

Primary admission: at least a 5% reduction in the median unprofiled
`wall_ns_per_item` for `modern/readrandom/4096`, computed from individual
iteration rows across the paired baseline/candidate repetitions.
Process CPU time is reported as supporting evidence, not substituted for
the primary wall-time metric.
Inspect random reads and scans at both sizes for non-target regressions.
A change smaller than measurement noise, or a repeated unexplained regression,
is not justified solely because its source looks simpler.

After unprofiled measurement, capture the candidate CPU profile to confirm
that the comparison cost moved as expected. Profiled timing is attribution
evidence only, not the reported speedup.

Merge only this optimization's code PR after its results and review. Then
reassess CRC32C against the new baseline before starting another change.

## Outcome

The clean baseline was built from `ca92618`; candidate `5041943` changed only
the comparator and added the independent ordering oracle. Both implementations
passed the expanded oracle, units, and ASan/UBSan correctness tiers.

Two paired rounds alternated baseline/candidate process order. Each process
used three repetitions calibrated from a 0.3-second minimum. Aggregating the
six individual wall-time samples for each variant gave:

| Case | Median wall-time reduction, candidate vs baseline |
|---|---:|
| `modern/readrandom/4096` | 3.56% |
| `modern/readrandom/65536` | -0.43% |
| `modern/readmissing/4096` | 3.48% |
| `modern/readmissing/65536` | 0.43% |
| `modern/scan/4096` | -1.30% |
| `modern/scan/65536` | -0.18% |
| `modern/seek_reuse/4096` | -21.36% |
| `modern/seek_reuse/65536` | -23.34% |

Negative reductions mean slower runs. The primary result did not reach the
predeclared 5% threshold, and the reused-seek cases showed substantial
regressions in this experiment. These results do not prove that `memcmp` is
universally slower: compiler/code-generation and fixture/background state
can affect the result. They do mean the evidence does not justify this patch.

Restore the original production comparator, retain the stronger correctness
oracle, and keep CRC32C as the next independent candidate. Do not retune
workloads or relax the admission metric merely to rescue this change.
Raw local results and executable digests are retained under
`build/bytewise-optimization/`.

## References

- [LevelDB bytewise comparator](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/comparator.cc)
- [LevelDB Slice comparison](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/slice.h)
- [RocksDB comparator](https://github.com/facebook/rocksdb/blob/v11.8.1/util/comparator.cc)
- [RocksDB Slice comparison](https://github.com/facebook/rocksdb/blob/v11.8.1/include/rocksdb/slice.h)
- [ADR-0042 profiling contract](0042-benchmark-profiling-foundation.md)
