# ADR-0085: Reviewed Read Optimization Adoption

- Status: Accepted
- Date: 2026-10-22

## Context

The retained `build/cache-read-candidates-20261022/report.json` experiment
contains 408 accepted timing processes, six fresh-process rounds per cell,
shared-database one/four-reader Get measurements, and separate cache, filter,
and storage-path measurements. The baseline is `e4029e8`. The inspected local
RocksDB revision is `553b52b4a709820fac1dbadc7796506740517582`.

The strongest whole-Get opportunity was excluding an absent SST candidate
before index seek. A scan-built full filter improved the measured corpus but
is not a production construction strategy. Retaining all 47 table readers
helped concurrent reads but is not a general file-resource policy.

Actual RocksDB CLOCK implementations and instruction-verified cache-local
Bloom probes established local opportunities, not an already-integrated Get
speedup. Unbuffered queue-depth measurements established storage throughput
opportunities, not individual Get latency. Changing replacement policy also
exposed large cyclic hot-set-shift regressions.

## Decision

Adopt the useful mechanisms in small, independently reviewed topics:

1. Persisted full-table, cache-local filters, with scalar behavior first.
2. Portable acceleration of that same new filter representation.
3. Resource-bounded reuse of live table readers.
4. A separately designed, read-efficient block-cache policy.
5. Bounded synchronous batch reads that can overlap independent Get I/O.

Keep implementation self-contained. Do not link RocksDB into the production
library, globally require AVX2, introduce a general coroutine runtime, or
change the WAL, MANIFEST, data-entry encoding, footer, or checksum algorithm.

Each topic needs a concrete design and an independent storage/system
architect persona review before production edits. After TDD implementation,
a different independent implementation-review persona checks the complete
bounded diff and evidence. Findings must be resolved before publication.
The roadmap does not pre-approve the later topics' concurrency designs.

### Default selection

Correctness is required for every path, not merely the default.
Measured improvements may become defaults; new options are not an excuse
to ship an unsafe alternative.

For filters, use an automatic mode that enables the new full filter for the
known built-in bytewise comparator. A custom comparator must not silently
inherit byte-equality filtering: automatic mode disables filtering for it.
Explicit filter selection retains the documented byte-equality obligation.
Explicitly unset `bloom_bits_per_key` continues to disable filtering.
The proposed default density is ten bits per key.

Do not change the default cache until the production implementation has
preserved identity, ownership, erase/replacement and capacity contracts and
has been measured for resident hits, pressure, concurrent reads, scans and
hot-set changes. In particular, do not adopt SIEVE, plain LFU or resident-only
LRU-2 as the default from the original uniform-Get result.

Existing individual Get calls remain synchronous and concurrently usable.
A new batch call must bound workers, join them before returning, retain one
read sequence, use independent outputs, preserve per-key errors, and avoid
using the serial maintenance executor as a read worker pool.

### Evidence and limits

Keep local-operation and whole-Get evidence distinct. Do not add independent
percentage gains. Retain original data and negative results, including the
discarded SIMD experiment whose shared inline symbols selected scalar code
for both paths.

Use fixed work, ordinary native/codec/checksum controls, one/four readers,
byte-budget and metadata accounting, and independent output verification.
Do not flush global OS caches or alter unrelated processes. Distinguish
buffered file reads from storage-path I/O and device-internal caches.

Default changes require a documented explanation of their affected workloads
and compatibility, not a preset speedup threshold. A reliable adverse path
must be addressed or explicitly kept out of the default.

## Delivery boundaries

The first concrete format design is
[ADR-0086](0086-persisted-full-table-filters.md). Its scalar implementation and
ISA acceleration are separate implementation commits and review boundaries.
Table-reader reuse, the cache backend and batch reads receive their own
concrete design records before implementation.

Update the API reference, architecture and relevant learning material with
each delivered behavior. Keep commits and pull requests topic-scoped. Use
ordinary CI and changed-code coverage gates without bypasses.

An independent storage-architect persona approved the bounded staging and
full-filter design. This accepts the review/selection framework, not a
future component design that has not yet been independently reviewed.

## Alternatives not selected

- Shipping the open-time full-SST scan prototype.
- Permanently pinning every table without accounting for file resources.
- Replacing the shared generic cache with a 16-byte-key-specific cache.
- Reinterpreting legacy Bloom bits using a different SIMD-friendly layout.
- Treating an asynchronous API name as proof of asynchronous execution.
- Treating simulated miss reduction as measured native CPU or Get latency.
