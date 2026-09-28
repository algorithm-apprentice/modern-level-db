# ADR-0052: Cross-Engine Point-Read Gap Decomposition

## Status

Superseded before tooling implementation by
[ADR-0053](0053-leveldb-read-path-parity.md). The preliminary matrix remains
useful historical evidence, but the project returned to completing the pinned
LevelDB read-path baseline instead of adding another attribution layer.

## Context

ADR-0049 established the fixed read workload, the pinned Google LevelDB
reference, the forced-`pread` control, and Modern-only fixed-count
diagnostics. ADR-0050 accepted explicit opt-in mmap reads. ADR-0051 then showed
that a mechanism can dominate an inclusive profile without explaining the
end-to-end gap: trusted comparison covered 91.25% through 94.87% of internal
comparisons but improved cache-fit point reads by only 1.01%.

The remaining question is therefore not which isolated function has the
largest inclusive stack. It is which exclusive CPU, allocation, and work-count
differences make one complete LevelDB `Get` cheaper than one complete Modern
LevelDB `Get`.

A preliminary single-process orientation run from the production code at
`ae317a3` used three individual repetitions but no paired-round protocol. It
is not final evidence:

| Workload | Matched access | Modern median | LevelDB median | Modern gap |
|---|---|---:|---:|---:|
| `readrandom/4096` | mmap | 505.23 ns/Get | 424.72 ns/Get | +80.51 ns / +18.96% |
| `readrandom/4096` | `pread` | 509.72 ns/Get | 429.91 ns/Get | +79.82 ns / +18.57% |
| `readrandom/65536` | mmap | 1,201.93 ns/Get | 980.41 ns/Get | +221.52 ns / +22.59% |
| `readrandom/65536` | `pread` | 1,489.57 ns/Get | 1,181.29 ns/Get | +308.28 ns / +26.10% |
| `readmissing/4096` | mmap | 493.64 ns/Get | 447.82 ns/Get | +45.83 ns / +10.23% |
| `readmissing/4096` | `pread` | 473.69 ns/Get | 460.78 ns/Get | +12.91 ns / +2.80% |
| `readmissing/65536` | mmap | 1,211.15 ns/Get | 1,147.85 ns/Get | +63.29 ns / +5.51% |
| `readmissing/65536` | `pread` | 1,624.60 ns/Get | 1,215.38 ns/Get | +409.22 ns / +33.67% |

The missing-read results visibly need paired confirmation. The matrix still
establishes two design requirements:

1. File access must be matched within each comparison. Modern default
   `pread` versus LevelDB default mmap is a product-default comparison, not an
   engine-core decomposition.
2. Present and missing reads must both remain visible. Their difference
   contains result ownership and buffer reuse, but also potentially different
   block-cache and key-placement behavior.

The 4,096-record corpus is a cache-fit control, not the target database size.
If an application's complete durable state is permanently that small and it
does not need database recovery, snapshots, ordered iteration, or compaction,
an in-memory map may be the better design. The control remains useful because
large databases often have a small hot working set and because it exposes
fixed per-Get CPU costs without block-cache pressure.

The 65,536-record corpus is the primary decomposition case. Its roughly
17 MiB of values exceeds the 8 MiB block cache and exercises block misses,
decompression, cache insertion, and table access. Both corpora remain warm
OS-page-cache workloads; neither claims cold-device behavior.

Existing evidence is insufficient for an additive budget:

- Modern diagnostics have exact foreground work counts and sampled stage
  timings, but LevelDB has neither.
- Time Profiler inclusive stacks overlap. Their percentages cannot be added.
- The current summaries retain only the top raw symbols, so they cannot
  partition every foreground sample into exclusive categories.
- The benchmark intentionally uses each public API's natural result
  ownership. That difference is real, but no symmetric allocation count
  currently quantifies it.
- LevelDB's lazy `Version::ForEachOverlapping` interleaves candidate traversal
  and file reads through a callback. Retrofitting Modern's stage boundaries
  would compare different operations.

## Decision

### Keep this slice diagnostic-only

Do not change production database behavior, public APIs, persisted bytes,
cache policy, or mmap defaults. The implementation PR adds only benchmark,
profiling, build-owned reference instrumentation, validation, and analysis
tooling.

Ordinary throughput evidence must continue to use the existing uninstrumented
`modern_leveldb_performance` executable. Diagnostic counts and allocation
tracking use a separate executable and are never throughput evidence.

### Compare two matched file-access modes

Use these names consistently:

| Mode | Modern LevelDB | Google LevelDB |
|---|---|---|
| `mapped` | `--modern-file-access mmap` | default POSIX access |
| `copied` | default `pread` | `--reference-file-access pread` |

The mapped comparison answers the residual gap after both engines use mapped
table files where available. The copied comparison answers the residual gap
after both engines use positioned reads.

Also report each engine's mapped-versus-copied delta. Do not subtract one
engine's access delta from the other engine's total and call the remainder an
exact algorithmic cost: mmap changes allocation, copying, cache insertion, and
page-fault behavior together.

Modern mmap remains opt-in during this experiment so the decomposition does
not change a public default while measuring it. The database directory is
single-process-owned while open, and live external modification of its files
is unsupported. Under that clarified contract, ADR-0050's default may be
revisited in a separate ADR after this task. The mapped/copied matrix provides
the evidence for that decision without combining it with diagnostic tooling.

### Add symmetric fixed-count diagnostics

Extend the dedicated diagnostic executable to accept both `modern` and
`leveldb` `readrandom`/`readmissing` cases. Keep the existing exact
4,194,304-operation foreground epoch, canonical corpus fingerprints, setup
exclusion, and one fresh process per case.

Version the report schema and emit the same counter names for both engines:

- Get outcomes: mutable, immutable, SSTable, deletion, and complete miss.
- Level-0 candidates, deeper-level candidates, and table files searched.
- Table-cache hits/misses and block-cache hits/misses/inserts.
- Random-read calls and requested/returned bytes.
- Stored blocks/bytes, mapped-source versus copied-source blocks, decoded
  blocks/bytes, and decompressions.
- Construction-time validation entries. LevelDB reports zero because its
  block reader validates entries lazily.
- Restart, index, and data entries decoded.
- Internal-key comparisons attributed to index seek, data seek, or other
  work, plus user-comparator calls.
- Result bytes and whether the destination capacity was reused or grew.
- Foreground allocation calls and requested bytes.

Require outcome counts to cover every Get and require every subtotal to match
its total. Normalize every count per Get while retaining raw integers.

Do not add reference stage timers. Exact work counts are comparable; timing
boundaries around LevelDB's callback-driven traversal would not be equivalent
to Modern's eager candidate-vector stages. Keep one versioned
`stage_timings` section with an explicit availability flag: Modern may retain
its existing sampled stages, while LevelDB reports them unavailable with this
reason. Cross-engine conclusions cannot use that section.

### Count allocations with one executable-level mechanism

Link the diagnostic executable, and only that executable, with replacement
global C++ allocation functions covering scalar, array, aligned, sized, and
nothrow forms. A foreground thread-local session records successful allocation
calls and requested bytes. It is inactive during setup, warmup, verification,
teardown, and every background thread. The report names these
`cxx_allocation_*`; it does not claim to observe direct C `malloc` calls.

This mechanism observes allocations made by both engines and their standard
library containers without modifying public database APIs. It deliberately
records requested rather than allocator-rounded bytes. Throughput binaries
must not link the replacement functions.

Tests must prove:

- Allocations outside a session are not counted.
- A known scalar, array, and aligned allocation is counted exactly once.
- Nothrow failure behavior remains standard-conforming.
- Deallocation and nested setup cannot leave a session active.
- The engine diagnostic epoch reports deterministic nonzero allocation work
  where the workload requires it.

### Instrument the authenticated reference only in a dedicated build

Add a `gap-diagnostics` preset inheriting the optimized profiling flags. Its
private cache option enables reference diagnostics. It produces a separately
named cross-engine diagnostic executable and no throughput executable. The
normal `profiling` build remains unchanged and is the only build used for
throughput and CPU profiles.

When the option is enabled:

1. Populate the pinned LevelDB archive in the build-owned `_deps` directory.
2. Reject a source override, as the existing forced-`pread` patch does.
3. Apply an idempotent CMake patch script that matches exact pinned source
   text and fails on source drift.
4. Compile the reference hooks and their implementation into that build's
   `leveldb` target.
5. Record the reference revision, patch SHA-256, compile flag, and diagnostic
   availability in every report.

The patch may add counter and role hooks only at these existing boundaries:

- `DBImpl::Get` for Get outcomes.
- `Version::ForEachOverlapping`, `Version::Get::Match`, and `SaveValue` for
  candidates, files searched, deletion/value outcomes, and result capacity.
- `TableCache::FindTable` for table-cache outcomes.
- `Table::BlockReader` and `Table::InternalGet` for block-cache outcomes and
  index/data seek roles.
- `ReadBlock` for stored-source, checksum/decompression, and decoded-block
  accounting.
- `Block::Iter` for decoded entries.
- `InternalKeyComparator::Compare` for role-attributed internal comparisons.

Hooks are foreground thread-local and perform integer increments only. They
must not change LevelDB ownership, caching, iterator behavior, or error
propagation.

Use adapter-level counting comparators for both engines to count user
comparisons symmetrically. Each wrapper forwards the exact underlying
comparator name and shortening behavior so persisted comparator identity and
table construction remain unchanged. Use a reference `EnvWrapper` to
cross-check random-access calls and bytes without replacing the underlying
mmap or `pread` implementation. Use a forwarding reference block-cache
wrapper to cross-check block-cache counts. Disagreement between wrapper and
internal hook totals is a diagnostic failure, not a fallback.

### Produce an exclusive sampled CPU budget

Extend the Time Profiler parser rather than post-processing only its top-50
symbols. Classify every deduplicated in-window target-process sample exactly
once using its complete stack and an engine-specific, versioned rule set.

The common categories are:

1. Candidate traversal and version lookup.
2. Table-cache lookup and lifetime.
3. Block-cache lookup, insertion, eviction, and lifetime.
4. File access and stored-block copying.
5. Checksum.
6. Decompression.
7. Block seek, key reconstruction, and comparison.
8. Result ownership and copy.
9. Locking, reference management, and database coordination.
10. Background flush/compaction work.
11. Other engine work.
12. Benchmark/runtime/unresolved work.

Rules use the most specific matching stack ancestor, not only the leaf symbol.
For example, allocator or `memmove` samples under Snappy belong to
decompression, while allocator samples under result assignment belong to
result ownership.

The summary records:

- Rule-set version and engine.
- Raw sample count and weight.
- Exclusive sample count/weight/percentage per category.
- Foreground/background sample weight per category.
- The most frequent leaf symbols assigned to each category.
- Unresolved-frame count.
- A proof that category weights sum exactly to the total in-window
  target-process sample weight.

For one profile only, multiply an exclusive category percentage by that same
profile run's process-CPU ns/Get to obtain a sampled CPU budget. Category
deltas then sum to that profile pair's process-CPU gap. Do not combine those
sample-derived nanoseconds with a different throughput run, call them wall
latency, or present them as request percentiles.

### Fix the evidence matrix before collection

Freeze one clean revision after the tooling PR merges. Retain separate build
directories and matching object files/symbol bundles until every capture is
complete.

Throughput:

- Cases: `readrandom` and `readmissing`, at 4,096 and 65,536 records.
- Treat 65,536 records as the primary database-path evidence and 4,096 as a
  cache-fit fixed-cost/hot-set control.
- Modes: `mapped` and `copied`.
- Engines: Modern and LevelDB.
- Three paired rounds, three individual repetitions, 0.3-second minimum.
- Alternate engine order in the middle round.
- Report wall ns/Get, process-CPU ns/Get, Get/s, per-round gaps, aggregate
  medians, and round spread.

Diagnostics:

- One fixed-count epoch for every throughput engine/case/mode combination.
- Sixteen fresh processes in total.
- Counts are deterministic contracts; any unexplained drift between paired
  engines or wrapper/internal cross-checks is investigated before profiles.

CPU profiles:

- Mapped `readrandom` and `readmissing` at both record counts for both engines:
  eight captures.
- Copied `readrandom` and `readmissing` at 65,536 records for both engines:
  four additional captures.
- Five measured seconds, one repetition, warning-free symbols, and the
  existing verified signpost window.

Do not add favorable cases, repeat only surprising profiles, or replace a
failed capture without retaining it. A failed or warning-bearing capture is
not evidence.

### Select the next candidate from triangulated evidence

For every engine/case/mode, publish:

- The paired wall and process-CPU gap.
- The exact counter and allocation deltas.
- The exclusive sampled CPU category budget and Modern-minus-LevelDB deltas.
- A short reconciliation of count, allocation, and profile evidence.

A category may nominate the next optimization only if:

1. Its sampled Modern-minus-LevelDB CPU delta is at least 25 ns/Get or 15% of
   the matched-mode process-CPU gap in a 65,536-record point-read case. A 4,096
   result may corroborate the mechanism but cannot nominate it alone.
2. Exact counts or allocation evidence identify a concrete mechanism rather
   than only a symbol family.
3. The direction is consistent with the present/missing and mapped/copied
   controls, or the workload-specific reason is explicit.
4. The candidate can be isolated from unrelated cache, API, file-access, and
   persisted-format changes.

Prefer the smallest private mechanism with the strongest evidence. If no
category meets these rules, stop without inventing an optimization. Any
selected mechanism requires its own ADR and admission gate.

## Validation plan

Before collecting evidence:

- Unit-test report schemas, duplicate/missing keys, normalization, and all
  subtotal invariants.
- Unit-test allocation replacement forms and inactive-session behavior.
- Unit-test reference hook sessions, role restoration, overflow assertions,
  and wrapper/internal cross-check failures.
- Test that ordinary throughput binaries contain no diagnostic or allocation
  symbols and reject diagnostic mode.
- Test that normal profiling builds report reference diagnostics unavailable.
- Test that source overrides cannot be patched and that pinned-source drift
  fails configuration.
- Run one-operation smoke diagnostics for both engines and both file-access
  modes.
- Run AppleClang and GCC warning-clean builds, ASan/UBSan, TSan, the existing
  compatibility/model tests, format fuzz smoke, and changed-code coverage.
- Use synthetic profiler stacks to test every category, precedence rule,
  unresolved symbol, duplicate sample, and exact partition invariant.

The final evidence collection begins only after one bounded GPT-5.6 Sol review
of the exact tooling and all applicable local gates pass.

## Non-goals

- Changing the mmap default in this diagnostic-only task. Revisit it
  separately under the exclusive live-file ownership contract.
- Making LevelDB artificially copy its reusable output.
- Disabling Modern's owning result to imitate LevelDB.
- Enabling external hardware CRC in the pinned LevelDB reference.
- Redesigning either cache.
- Implementing lazy candidates, inline keys, trusted comparison, reusable
  output, or any other production optimization.
- Claiming cold-device I/O, tail latency, concurrent saturation, or universal
  architecture results.

## Consequences

- The normal benchmark remains the throughput authority.
- The dedicated diagnostic build becomes cross-engine and more invasive, but
  only against authenticated build-owned reference source.
- Allocation counts quantify API and container ownership without changing
  either API.
- Profile categories provide an additive sampled CPU budget while preserving
  raw symbols and explicit uncertainty.
- The result should select one next experiment or explicitly conclude that no
  sufficiently attributable private mechanism remains.

## Delivery boundary

Merge this design-only PR after one bounded GPT-5.6 Sol review. Implement the
tooling in a separate sequential PR. Freeze and collect evidence only from the
merged tooling revision, then record the analysis and selected next candidate
in a final documentation PR.

## References

- [ADR-0042 benchmark and profiling foundation](0042-benchmark-profiling-foundation.md)
- [ADR-0049 LevelDB-style point-read baseline](0049-leveldb-style-point-read-baseline.md)
- [ADR-0050 opt-in POSIX mmap reads](0050-posix-mmap-table-reads.md)
- [ADR-0051 trusted comparison outcome](0051-trusted-internal-key-comparison.md)
- [Profiling infrastructure](../profiling-design.md)
- [Pinned LevelDB `DBImpl::Get`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [Pinned LevelDB version lookup](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [Pinned LevelDB table reader](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/table.cc)
- [Pinned LevelDB block reader](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/format.cc)
