# ADR-0050: POSIX Mmap-Backed Table Reads

## Status

Accepted. Design PR #56 merged before the separately reviewed and measured
implementation experiment.

## Context

ADR-0049 separated file-access policy from the other point-read costs. After
the accepted structural-validation change, the retained measurements are:

| 65,536-record case | Modern Get/s | LevelDB `pread` Get/s | LevelDB mmap Get/s | Modern latency vs LevelDB `pread` | Modern latency vs LevelDB mmap |
|---|---:|---:|---:|---:|---:|
| `readrandom` | 660,397 | 751,982 | 951,299 | +13.87% | +44.05% |
| `readmissing` | 687,037 | 802,479 | 987,636 | +16.80% | +43.75% |

Within the pinned reference, forcing `pread` instead of its default mmap path
adds 23-27% latency in these warm page-cache cases. This does not predict an
identical Modern gain, but it proves that access policy is a material part of
the observed gap.

Modern diagnostics report about 0.55 data-block cache misses and `pread` calls
per Get at 65,536 records. Every stored block read in those diagnostic epochs
was Snappy-compressed. The sampled `pread` operation averaged about 431-438 ns
per miss after the structural-validation change. These are inclusive
diagnostic timings, not additive speedup promises.

The current POSIX `RandomAccessFile` always copies through a caller-owned
buffer. Merely replacing its implementation with mmap would still copy mapped
bytes into that buffer and would retain most of the avoidable work. The
smallest useful change is therefore an optional borrowed-view capability that
lets the table reader checksum and decompress directly from a stable mapping.

## Prior art and platform constraints

### Google LevelDB

The pinned LevelDB reference maps up to 1,000 read-only files on 64-bit POSIX
and maps none on 32-bit. Once that limiter is exhausted it uses its existing
`pread` file implementation. A successful mapping owns the file's full
current length, closes the descriptor, supports concurrent immutable reads,
and releases its limiter slot on `munmap`.

LevelDB's block reader accepts either caller scratch or a borrowed file view.
For an uncompressed mapped block it borrows the mapping and avoids the block
cache. For a compressed block it decompresses directly from the mapping into
owned output.

Adopt the full-file mapping, bounded resource use, descriptor closure, and
direct compressed-input decode. Do not adopt borrowed uncompressed `Block`
storage in this experiment; it would change block-cache ownership and
lifetime contracts at the same time.

### RocksDB

RocksDB exposes `allow_mmap_reads`, but its default is false. Its documented
mapped uncompressed path bypasses the block cache and may verify the checksum
on every read. That broader policy is not the baseline here. It confirms that
mmap has meaningful ownership, caching, and failure tradeoffs that must remain
explicit.

### POSIX

POSIX permits closing a descriptor after a successful mmap; the mapping keeps
its own reference to the file. Mapping length must be nonzero. The behavior of
accesses after the underlying file size changes is unspecified, and accesses
to pages removed by truncation may deliver `SIGBUS`.

Modern LevelDB never truncates a live SSTable. Obsolete-file cleanup removes a
table from the table cache before unlinking it, and live-file tracking prevents
new handles. Unlinking a mapped file is safe on the supported POSIX systems;
the mapped file object is normally destroyed and unmapped before that unlink.
Concurrent external modification or truncation of a live table is outside the
database's existing immutable-file protocol and becomes an explicit mmap
precondition. A storage error raised while faulting a mapped page may also be
delivered as `SIGBUS` rather than the typed `Io` result available from
`pread`; this is part of the candidate's admission tradeoff.

Because that failure-mode change cannot be validated away with a throughput
test, mmap reads are explicit opt-in behavior. RocksDB makes the same default
choice.

## Decision

### Add an optional exact borrowed view

Extend the internal `RandomAccessFile` interface:

```cpp
class RandomAccessFile {
 public:
  virtual Result<std::size_t> Read(std::uint64_t offset,
                                   MutableByteView output) const = 0;

  virtual std::optional<ByteView> TryReadView(
      std::uint64_t offset, std::size_t size) const noexcept {
    return std::nullopt;
  }
};
```

- A returned view contains exactly `size` bytes.
- The view remains valid and immutable until the file object is destroyed.
- Concurrent calls do not invalidate existing views.
- `std::nullopt` means that no exact stable view is available; callers retain
  the existing `Read` path and short-read/error behavior.
- The default implementation returns `nullopt`, so custom and in-memory file
  systems need no change.
- This capability is internal. Do not expose mapping or borrowed file bytes in
  the public database API.

The table reader calls `TryReadView` only after validating the requested block
range against the MANIFEST file size. A mapped file also checks its actual
mapped length. If the exact view is unavailable because the actual file is
shorter, the existing copied read reports truncation.

Extend the internal open call to accept an optional expected size:

```cpp
virtual Result<std::unique_ptr<RandomAccessFile>> OpenRandomAccess(
    const std::filesystem::path& path,
    std::optional<std::uint64_t> expected_size = std::nullopt);
```

The table cache passes the MANIFEST file size. Other callers and custom file
systems may ignore an absent hint and retain copied reads. The POSIX backend
maps only when the actual `fstat` size exactly equals the expected size. A
shorter or longer actual file selects `pread`; this preserves typed truncation
behavior and prevents an unexpectedly enlarged file from consuming mapping
budget.

### Require explicit public opt-in

Add:

```cpp
struct Options {
  bool allow_mmap_reads = false;
};
```

The engine forwards it only to its owned default POSIX filesystem. `false`
preserves the existing typed-I/O behavior and performs no mapping. A caller
that supplies the internal custom filesystem seam controls that filesystem's
own random-access capabilities.

The performance candidate explicitly sets this option to true and records
`modern_file_access=mmap`. Do not silently benchmark a different default.

### Map bounded full files in the POSIX backend

Add a read-only mapped `RandomAccessFile` implementation:

- Use `mmap(nullptr, length, PROT_READ, MAP_SHARED, descriptor, 0)`.
- Map the full actual file length obtained with `fstat`.
- Close the descriptor after successful mapping.
- `Read` remains supported by copying an in-range portion from the mapping,
  preserving the existing filesystem contract for footer and other copied
  reads.
- `TryReadView` returns an exact in-range span without copying.
- The destructor calls `munmap` and releases one limiter slot.

All default `PosixFileSystem` instances share one process-wide mapping budget
that open mapped files keep alive:

- At most 1,000 concurrent mappings on 64-bit, matching LevelDB's count.
- At most 4 GiB of concurrently mapped file bytes.
- No mappings on 32-bit.

The byte budget is approximately twice the default table cache's maximum
target footprint: 990 cached tables at the default 2 MiB target file size.
Larger configured table files consume proportionally more of the same budget
and therefore fall back sooner. This deliberately differs from LevelDB's
count-only singleton budget and prevents several databases or large/sparse
files from reserving unbounded virtual address space.

Tests may inject an isolated count/byte budget. Production callers cannot
configure it in this slice; resource exhaustion is a performance fallback,
not a correctness failure.

`OpenRandomAccess` keeps the existing `pread` implementation when:

- The limit is zero or exhausted.
- The total-byte budget cannot reserve the expected file size.
- No expected size was provided, or the actual and expected sizes differ.
- The file is empty.
- Its size is negative or cannot fit `std::size_t`.
- `fstat` or `mmap` fails.

Mapping is an optimization, not a new open failure. Every such condition
falls back to the already opened descriptor and existing typed read errors.
Do not add `madvise`, `posix_madvise`, `MAP_POPULATE`, `mlock`, readahead, or
direct I/O in this slice.

### Decode stored blocks from either ownership form

Add a `DecodeStoredBlock(ByteView)` overload beside the existing owning-vector
overload. Both paths:

- Require the trailer.
- Verify the checksum before interpreting the compression type.
- Reject unknown types and malformed compressed data as today.

The owning-vector overload continues to reuse its input allocation for
uncompressed blocks. The borrowed-view overload:

- Copies uncompressed contents into an owning vector, preserving current
  `Block` and block-cache lifetime semantics.
- Decompresses Snappy or Zstd directly from the mapped bytes into owned
  output, avoiding the `pread` syscall and intermediate stored-byte copy.

`ReadStoredBlock` prefers an exact borrowed view and otherwise runs the
unchanged allocate/`ReadExactly` path. Footer reads and every custom
filesystem continue to work through `Read`.

This experiment intentionally does not add a borrowed `Block`, pin a table
mapping in the block cache, bypass the block cache for uncompressed data, or
change cache charge.

### Preserve deletion and ownership order

The table owns its random-access file and every table operation already holds
a table-cache handle. A borrowed stored-block view is consumed synchronously
before that handle can be released.

Obsolete-file cleanup retains its existing order:

```text
prove file not live or pending
→ evict table cache entry
→ destroy/unmap the unreferenced table
→ unlink the path
```

No new deferred unmap queue or mapping registry is needed. If a violated
engine invariant leaves a table handle alive during unlink, POSIX keeps that
mapping valid until the handle is destroyed; the path removal does not
truncate the mapped object.

### Record attribution without timing page faults as a new stage

Extend the diagnostic schema with two sections.

One setup/open snapshot spans fixture creation, reopen, verification, and
warmup and records each random-access file decision:

- Mapped successfully.
- mmap disabled.
- Missing expected size.
- Empty file.
- Actual/expected size mismatch.
- File too large for `std::size_t`.
- Count-budget exhausted.
- Byte-budget exhausted.
- `fstat` failure.
- `mmap` failure.

These are setup counters, not timed operations and not part of the fixed Get
epoch. Each file records exactly one selected access mode and fallback reason.

The existing foreground epoch adds mapped-view and copied-read block/count and
byte totals. During the fixed 65,536-record epoch:

- Stored-block `pread` calls should fall from about 0.55/Get to zero or a
  fallback count exactly explained by the setup snapshot.
- Mapped-view hits should account for the corresponding stored blocks.
- `stored_blocks == mapped_view_blocks + copied_read_blocks`.
- `decoded_blocks == stored_blocks`.
- For the frozen Snappy corpus, `decompressed_blocks == stored_blocks`.
- Every mapped-view miss has one persisted file fallback reason.

Compared with the frozen baseline diagnostic, normalized block-cache misses,
stored blocks, decompressions, decoded bytes, and validation entries may vary
by at most 2%. A larger change means the runs exercised different storage
work and cannot attribute throughput to mmap.

Do not introduce a separate "mmap latency" timer. Page-fault and mapped-byte
access cost occurs while checksum/decompression code touches the view, so such
a timer would misattribute work. Uninstrumented end-to-end throughput remains
the admission evidence.

## Validation plan

### Filesystem and lifetime tests

POSIX tests cover:

- Exact mapped views, copied reads, short reads, EOF, and concurrent reads.
- A zero mapping limit selecting `pread`.
- Count- and byte-budget exhaustion selecting `pread`, and destruction
  releasing both reservations for a later mapped file.
- Several default filesystems sharing one production budget.
- Empty files and forced mapping-unavailable conditions using the fallback.
- Actual files shorter and longer than the expected MANIFEST size selecting
  copied reads without mapping extra bytes.
- A mapped file remaining readable after its path is unlinked.
- Mapping and unmapping without leaking descriptors or limiter slots.

Do not truncate a live mapping in-process to test `SIGBUS`; that signal is not
a recoverable unit-test error. Document and review the external-mutation
precondition instead.

### Block and table tests

- Both stored-block decode ownership forms produce identical bytes and errors
  for none, Snappy, Zstd, checksum mismatch, unknown type, and malformed data.
- A table file whose copied `Read` fails still opens and reads through an exact
  mapped view.
- A short actual file cannot produce an out-of-range mapped view and remains a
  typed truncation error.
- Cached blocks remain valid after table eviction because mapped
  uncompressed input is copied into the existing owning `Block`.
- Existing model, compatibility, sanitizer, and fuzz gates remain unchanged.

Use the existing changed-code coverage policy. Resource-dependent real mmap
failures that cannot be injected without a new syscall abstraction may use a
narrow justified exclusion; do not add a general filesystem mock layer for
one branch.

## Measurement and admission

Merge this design, then freeze a new clean baseline that includes the accepted
structural-validation implementation. Keep the mmap candidate local and
uncommitted through review and measurement.

Use three paired rounds with three individual repetitions and a 0.3-second
minimum per fresh process. Run all eight Modern read-family cases, baseline
then candidate in rounds one and three and reversed in round two.

The primary case is `modern/readrandom/65536`. Require:

- At least 10% higher aggregate median Get/s in the primary case.
- Positive primary improvement in every paired round.
- No aggregate regression above 2% in `modern/readrandom/4096`.
- No other read-family case regresses by more than 5% in aggregate or by more
  than 5% in at least two rounds.
- The mapped-view diagnostic invariants above hold.
- Correctness, sanitizer, compatibility, fuzz, platform, and changed-code
  coverage gates pass.

The higher 10% primary threshold reflects the larger runtime-safety tradeoff:
mapped page faults can no longer always become typed `Io` errors if an
external actor mutates or truncates a live table. Users accept that tradeoff
only through `allow_mmap_reads=true`. The LevelDB mmap/`pread` control suggests
enough headroom to justify the threshold.

For each variant/case, compute round spread as
`max(round median Get/s) / min(round median Get/s) - 1`. If either variant's
spread exceeds 10%, that case's adaptive result is inconclusive. Do not select
favorable samples. Run exactly two fixed-work pairs for that case:

1. Candidate then baseline.
2. Baseline then candidate.

Each process performs 262,144 iterations and five repetitions. Use the median
individual Get/s of each process to calculate that pair's gain. For the
aggregate, pool all ten individual Get/s values of each variant, take each
variant's median, and calculate `candidate_median / baseline_median - 1`.
Retain every sample.

- For the primary case, the ten-sample aggregate gain must be at least 10% and
  both paired gains must be positive.
- For `readrandom/4096`, the aggregate regression must be no more than 2%.
- For another control, the aggregate regression must be no more than 5%, and
  reject only if both paired gains are below -5%. One bad pair alone is
  permitted only when the pooled aggregate still passes.

Confirmation replaces only that unstable case's adaptive result. Every stable
case and all correctness/diagnostic gates remain binding. A primary result
that still fails these fixed criteria rejects the candidate.

If the candidate passes, commit the exact measured engine patch and update
ADR-0011, ADR-0025, and this ADR with the accepted contracts and outcome. If
it fails, restore production and record the complete result without adding a
different default or tuning the mapping budgets to rescue it.

## Deferred work

- Borrowed uncompressed blocks and block-cache pinning of mappings.
- Configuration of the process-global mapping count or byte budget.
- Partial-file or per-block mappings.
- `madvise`, prefetch, readahead, page locking, direct I/O, and async I/O.
- Windows memory-mapped files.
- Recovery from external truncation or storage faults delivered as `SIGBUS`.
- Cold-device, concurrent saturation, and tail-latency claims.

## Consequences

- The design adds no engine behavior until the separate candidate is admitted.
- The planned candidate targets the measured compressed cache-miss path
  without changing persisted bytes or block ownership. It adds one public
  opt-in boolean because the failure semantics differ.
- Static file corruption remains a typed checksum/decompression/structure
  error. Concurrent external mutation of a live mapped table is explicitly
  outside the supported protocol, and a mapped page-fault storage error may
  terminate the process with `SIGBUS`.
- Mapping-resource exhaustion and mmap failures retain the existing `pread`
  behavior.
- A successful result would provide an explicit 64-bit POSIX mmap mode aligned
  with LevelDB's access strategy while deliberately retaining Modern's
  block-cache ownership and pread default.

## Outcome

The design merged at `ba151e5`. The candidate remained local and uncommitted
through correctness checks, bounded GPT-5.6 Sol review, the fixed throughput
experiment, and diagnostic comparison.

| Artifact | SHA-256 |
|---|---|
| Candidate source patch | `524d1ac60d984e3db8cf815fc29b875bf590d7fac9ba6780b747cea0b1f5ecf4` |
| Candidate throughput executable | `f85147af3c3b5059c261fedf24d64343d48f33190d3e35eb8852f7e0c0f23444` |
| Candidate diagnostic executable | `b2191015b636a63fa64efe4747c6ec22f2b49e1d88a38a8af3c73e425ec72f14` |

All 48 predetermined adaptive processes completed. The nine individual
samples per variant/case gave:

| Case | Baseline items/s | Candidate items/s | Adaptive gain |
|---|---:|---:|---:|
| `modern/readrandom/4096` | 2,008,908 | 1,999,666 | -0.46% |
| `modern/readrandom/65536` | 686,714 | 804,335 | +17.13% |
| `modern/readmissing/4096` | 2,110,547 | 2,109,161 | -0.07% |
| `modern/readmissing/65536` | 705,391 | 838,903 | +18.93% |
| `modern/scan/4096` | 21,917,920 | 21,538,232 | -1.73% |
| `modern/scan/65536` | 7,601,620 | 9,428,217 | +24.03% |
| `modern/seek_reuse/4096` | 346,119 | 342,566 | -1.03% |
| `modern/seek_reuse/65536` | 206,407 | 231,917 | +12.36% |

The primary round gains were +4.99%, +21.01%, and +21.90%. Its candidate
round spread, `scan/4096` candidate spread, and both variants of
`seek_reuse/65536` triggered the predeclared fixed-work confirmation. Each
process used 262,144 iterations and five repetitions, with both execution
orders:

| Case | Pair 1 gain | Pair 2 gain | Pooled gain | Result |
|---|---:|---:|---:|---|
| `modern/readrandom/65536` | +20.57% | +20.83% | +21.02% | Pass |
| `modern/scan/4096` | -2.57% | -15.84% | +6.09% | Control pass |
| `modern/seek_reuse/65536` | +19.69% | +12.46% | +16.11% | Pass |

The `scan/4096` confirmation remains noisy and does not establish a cache-fit
scan gain. It passes only its predeclared control rule: pooled throughput did
not regress, and fewer than both pairs regressed by more than 5%. No sample
was discarded or replaced.

Every candidate diagnostic file open mapped successfully: 8 files for the
4,096-record cases and 12-13 files for the 65,536-record cases. Every stored
block used a mapped view, copied-read blocks and `pread` calls were zero, and
every stored block was decompressed. Against the frozen baseline, the
65,536-record normalized work differed by only:

| Case | Block misses/stored/decompressed/validation drift |
|---|---:|
| `readrandom/65536` | +0.17% |
| `readmissing/65536` | +0.12% |

The exact candidate passed 556 unit tests, targeted ASan/UBSan and TSan tests,
the LevelDB compatibility model, the 1,000-input format fuzz smoke,
AppleClang/GCC 16 warning-clean builds, local changed-code coverage checks, and
bounded implementation review. The review added exception-safe mapping
reservation RAII, strict compression-work comparison, a real POSIX truncation
test, and proof that an owning cached block survives table unmap and unlink.

The candidate is accepted. `allow_mmap_reads` remains false by default. When
enabled on 64-bit POSIX, exact-size table files use the shared 1,000-map/4-GiB
budget and fall back to `pread` on every unavailable-resource or size-mismatch
condition. Raw patch, binary, adaptive, fixed-work, and diagnostic artifacts
remain local under `build/mmap-experiment-ba151e5/`.

ADR-0056 supersedes the final product policy while retaining this experiment's
evidence: mmap is now the default, the limiter is count-only, mapped
uncompressed blocks remain borrowed and bypass the block cache, and false is
the explicit copied-read control.

## Delivery boundary

Obtain one bounded GPT-5.6 Sol design review and merge this design-only PR
before implementation. Review the exact local candidate before measurement;
commit it only after local admission. Keep trusted comparators, lazy file
selection, cache redesign, output ownership, and prefetch separate.

## References

- [ADR-0011 filesystem contracts](0011-filesystem-contracts.md)
- [ADR-0025 SSTable reader](0025-sstable-reader.md)
- [ADR-0026 table cache and deletion order](0026-table-cache.md)
- [ADR-0049 point-read attribution and retained baseline](0049-leveldb-style-point-read-baseline.md)
- [Pinned LevelDB POSIX files](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/env_posix.cc)
- [Pinned LevelDB stored-block reading](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/format.cc)
- [RocksDB `allow_mmap_reads`](https://github.com/facebook/rocksdb/blob/main/include/rocksdb/options.h)
- [POSIX `mmap`](https://pubs.opengroup.org/onlinepubs/9799919799/functions/mmap.html)
- [Linux `mmap(2)`](https://man7.org/linux/man-pages/man2/mmap.2.html)
