# ADR-0044: Profile-Guided CRC32C Acceleration

## Status

Accepted; merge this design before its separate implementation PR.

## Evidence and prior art

After ADR-0043's comparator candidate was rejected, production comparison
remains unchanged. The next independent candidate is CRC32C:

- Cache-pressure random reads: `ExtendCrc32c` used about 42.6% self and 44.2%
  inclusive CPU sample weight.
- Cache-pressure scans: about 49.6% self and 51.6% inclusive.
- Cached 4,096-record reads are a control case with little checksum work.

These measured intervals come from ADR-0042's matched 8 MiB-cache, Snappy,
65,536-record workloads. Percentages identify a candidate; only separate
unprofiled before/after measurements admit the optimization.

Google LevelDB optionally delegates to Google CRC32C. RocksDB also uses
hardware-capability-selected CRC paths. Google CRC32C was extracted from
LevelDB and supplies x86 SSE4.2, ARM64 CRC, and optimized portable
implementations behind one stable incremental-checksum interface.

Use Google CRC32C commit
`2bbb3be42e20a0e6c0f7b39dc07dc863d9ffbc07` (2025-04-07), not a claim of a new
numbered release. Its last tag, 1.1.2, predates the unaligned/strict-aliasing
ARM64 correction and CMake 4 compatibility fix. The reviewed post-tag delta
contains those relevant fixes plus minor interface/comment/build updates.

An isolated pre-integration build at this exact commit passed independent
bitwise checks for aligned/unaligned buffers, lengths through 64 KiB, and
arbitrary initial CRC states. A second build with both accelerated paths
disabled passed the same checks. No production code was changed for those
probes.

## Decision

### Private dependency

Reuse an explicitly parent-provided `Crc32c::crc32c` or `crc32c` target, recording
that provider rather than claiming it is our pinned source. Otherwise,
FetchContent obtains the fixed commit and builds it as a private static
dependency. No codec header enters our public headers.

Dependency tests, benchmarks, glog, and installation default off in the
configuration scope. Do not overwrite parent cache settings or let dependency
settings disable our GoogleTest/RTTI configuration. Only CRC's specialized
object targets receive ISA-specific compile flags; do not add `-march=native`
or SSE/ARM requirements to the whole library.

Reject a predeclared private FetchContent name that could silently replace the
pin. Explicit source overrides are recorded as overrides. Parent target and
source provenance, requested revision, and known compiled acceleration
capabilities are included in profiling context.

The public build now has this additional private dependency even when tests
are disabled. This is justified by the observed cost, not introduced as a
speculative option family.

### Adapter semantics

Keep `Crc32c`, `ExtendCrc32c`, `MaskCrc32c`, and `UnmaskCrc32c` unchanged as public
interfaces, including `noexcept`.

`ExtendCrc32c` returns the input CRC immediately for an empty byte view,
including one with null storage. Otherwise it forwards the finalized CRC and
the nonempty byte buffer to `::crc32c::Extend`. The selected implementation is
allocation-free and performs only checksum and CPU-capability work; no new
normal exception path is introduced.

Keep masking/unmasking exactly as before. Do not change polynomials, checksum
coverage, trailer bytes, WAL framing, checksum verification policy, comparator
behavior, or snapshot semantics. Hardware selection happens at runtime through
the dependency's tested capability checks, with a portable fallback.

### Baseline and fairness

Freeze a clean baseline executable after the design merge, with the original
bytewise comparator and original CRC implementation. Preserve its digest and
build metadata. Alternate frozen baseline and candidate processes using
ADR-0042's identical data, options, and query sequences.

Primary admission requires at least a **20% reduction in median unprofiled
`wall_ns_per_item` in each** of `modern/readrandom/65536` and
`modern/scan/65536`, aggregating individual repetitions across paired rounds.
Report process CPU as supporting evidence and inspect cache-fit read/scan and
other selected workloads for repeated non-target regressions.

The existing LevelDB reference remains configured with its optional external
CRC library disabled. Do not move that baseline while testing this change.
Claims are Modern-before versus Modern-after; they are not claims to beat
the fastest hardware-enabled LevelDB configuration.

Capture candidate CPU intervals after unprofiled measurements to check that
checksum cost actually decreases. If admission fails, restore the original
implementation rather than loosening thresholds or changing workloads.

## Correctness and integration checks

- Preserve independent RFC/LevelDB golden vectors and mask values.
- Expand incremental and arbitrary-seed checks across word/stride boundaries,
  unaligned offsets, and larger practical blocks, using the independent
  bitwise oracle.
- Exercise the wrapper with empty/null views and input that must remain
  unchanged.
- Build a forced-portable configuration and run the focused CRC suite, in
  addition to the default runtime-dispatch paths.
- Verify parent-target reuse and default fetched builds without dependency
  tests, glog, benchmark targets, global ISA flags, or altered parent settings.
- Run existing unit, format/differential, crash, sanitizer, fuzz, platform, and
  changed-code coverage gates. No compatibility migration is needed because
  checksum values and persistent bytes are identical.
- Keep the legacy benchmark gate and profiling workload definitions unchanged.

## Scope

This PR changes only checksum implementation/dependency wiring and directly
related tests/provenance/docs. Do not add mmap, change block validation, replace
the cache, or revisit the rejected comparator in the same change.

## References

- [Google CRC32C source and dispatch](https://github.com/google/crc32c/tree/2bbb3be42e20a0e6c0f7b39dc07dc863d9ffbc07)
- [Reviewed changes after 1.1.2](https://github.com/google/crc32c/compare/02e65f4fd3065d27b2e29324800ca6d04df16126...2bbb3be42e20a0e6c0f7b39dc07dc863d9ffbc07)
- [LevelDB accelerated CRC hook](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/port/port_stdcxx.h)
- [ADR-0042 measurement contract](0042-benchmark-profiling-foundation.md)
- [ADR-0043 rejected comparison experiment](0043-profile-guided-bytewise-comparison.md)
