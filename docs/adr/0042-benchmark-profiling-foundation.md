# ADR-0042: Benchmark and Profiling Foundation

## Status

Accepted for the next infrastructure slice; implementation has not started.

## Context

ADR-0041 completed the initial MVP and a deliberately small comparative
benchmark. The next task is to explain measured read-path differences before
choosing an optimization. It is not to make the engine resemble RocksDB by
copying options without their prerequisites.

The existing benchmark measures multiple phases and implementations in one
process. It has a useful severe-regression gate, but it is not a general
performance framework or an operation-latency histogram.

The research and concrete pre-implementation checks are recorded in
[Profiling research and implementation plan](../profiling-design.md).

## Decision

### Scope and dependencies

Add an optional development-only Google Benchmark executable and a result
collector. Keep `modern_leveldb_bench`, its JSON schema, its 20x guard, and
the existing correctness/sanitizer/fuzz gates unchanged.

Use Google Benchmark **v1.9.5**, commit
`192ef10025eb2c4cdd392bc502f0c852196baa48`. Use its timing, iteration
calibration, repetitions, counters, and JSON reporting; do not create a second
measurement/statistics framework.

The new `MODERN_LEVELDB_BUILD_PERFORMANCE_TESTS` option defaults off.
Enabling it requires the POSIX database backend and fetches the pinned
framework only when requested. Its own tests, assembly tests, documentation,
and install rules default off without forcing parent cache settings.
Framework source and license notices remain with the dependency.

The development harness requires the pinned framework rather than silently
substituting an arbitrary existing `benchmark` or `benchmark::benchmark`
target. A collision fails explicitly, as the pinned LevelDB oracle already
does. The normal library build remains unaffected when the option is off.
An explicit FetchContent source override is recorded as an override, not
misrepresented as verified source at the pinned revision.

Reuse the root-owned LevelDB reference and codec targets; do not create
another LevelDB build or disable its RTTI. RocksDB, YCSB, JNI, a new allocator,
and libpfm are not initial dependencies.

### Workloads and comparable settings

The first family contains sixteen cases: two implementations, two data
sizes, and four read workloads.

| Dimension | Values |
|---|---|
| Implementation | Modern LevelDB; pinned Google LevelDB |
| Records | 4,096; 65,536 |
| Value size | 256 bytes |
| Workloads | `readrandom`, `readmissing`, `scan`, `seek_reuse` |
| Foreground threads | One |
| Seeds | Data 301; insertion 302; present queries 303; missing queries 304 |
| Compression | Snappy, using the same codec build |
| Block cache | 8 MiB in each engine |
| Write buffer | 64 KiB |
| Data blocks | 4 KiB; restart interval 16 |
| Filter policy | None in both engines |
| WAL / sync | WAL enabled; preparation writes use `sync=false` |
| Read checksums | Enabled in LevelDB; always enabled in Modern LevelDB |
| Hardware CRC library | Reference remains disabled under ADR-0041 |

These are approximately 1 MiB and 16 MiB of value data before key/block
overhead. Call them cache-fit and cache-pressure workloads, **not cold-disk
workloads**: the OS page cache is not purged.

Use deterministic, unique ordered keys for even numeric positions. Missing
queries use interleaved odd positions, not a prefix beyond every table's
largest key. This prevents a negative-read case from measuring only a trivial
out-of-range rejection.

Values are three-quarters repetitive and one-quarter pseudorandom. Generate
records, insertion order, and query order once, before timing. Use the specified
`std::mt19937_64` sequence and an explicit Fisher-Yates shuffle; do not rely
on `std::shuffle` or `std::uniform_int_distribution` producing identical
permutations across standard libraries. The exact key encoding, random-byte
mapping, rejection sampling, and seed streams are fixed in
[the canonical generator](../profiling-design.md#canonical-corpus-and-orders).
Insert in that fixed shuffled order, not sorted order. Record separate
CRC32C fingerprints of records, insertion order, and both query orders. These
are reproducibility checks, not cryptographic integrity claims.

Reuse LevelDB's workload concepts, not its whole driver or unsupported APIs.
`seek_reuse` deliberately keeps one iterator and seeks repeatedly; it is a
diagnostic workload and is not advertised as an identical implementation of
upstream `seekrandom`.

Use each public API naturally. In particular, the LevelDB adapter may reuse
its output string, while Modern LevelDB returns its owning result. Do not add
an extra copy to either adapter to make their allocation counts look equal.
Consequently these measurements are a **new baseline**, not a continuation
of the legacy benchmark's exact ratios.

### Fixture lifecycle and timing

Use a small compile-time adapter for each public API, sharing data generation,
query order, operation semantics, and verification. No owning raw pointers
escape the upstream API wrapper.

The executable accepts one explicit case identifier and registers only that
case. Its custom main owns the fixture context; the callback initializes it
lazily on its first invocation and reuses it through Google Benchmark
calibration and repetitions. This gives:

1. No data preparation for listing/help/invalid selections.
2. One dataset preparation per process, not one per calibration attempt.
3. No second engine doing background work while process CPU time is measured.

The runner creates a fresh artifact directory and owns a `work/` subdirectory;
it passes a nonexistent `work/db` path to the executable. No user database is
opened, and no data directory is created by help, listing, or invalid case
selection. Preparation inserts the canonical sequence, closes and reopens the
database, and verifies its complete contents with `fill_cache=false`.

Next, perform exactly one untimed pass of the selected workload with
`fill_cache=true`, in its fixed query order, or one full traversal for `scan`.
Reset the query cursor to zero before each callback's measured loop, without
resetting or clearing caches between calibration attempts or repetitions.
`seek_reuse` retains one iterator across callbacks; rewind it to the first
entry outside timing before each callback so its initial position is defined.

Exactly once after `RunSpecifiedBenchmarks()` returns, main releases retained
query handles, performs final full-content/order verification with
`fill_cache=false`, and explicitly destroys the database. A verification or
observable artifact/cleanup failure causes a failed run, not an exception
hidden in a destructor. Database destruction retains ADR-0038's existing
best-effort close contract; the harness cannot invent a close status that the
public API does not return. The runner removes its `work/` directory only
after every owned process has terminated. Both failed and successful runs
retain their report and diagnostic artifacts.

Automatic compactions are not disabled or treated as quiescent by assumption.
Both implementations run their normal background policies. The artifact
records that quiescence was not forced, and background samples are analyzed
separately from foreground stacks.

Register each case with `UseRealTime()` and `MeasureProcessCPUTime()`.
Report wall time and whole-process CPU time; the latter includes the engine's
background threads and may exceed wall time. Do not present thread-only CPU
time as total engine CPU.

One iteration is one Get for random reads, one seek/check for `seek_reuse`,
and one complete traversal for `scan`. Each scan iteration creates a new
iterator, seeks to the first entry, traverses, checks terminal status and
cardinality, and destroys the iterator, all inside the measured iteration.
It does not reuse a retained iterator across scans.
Report `items_per_iteration` and
`SetItemsProcessed` explicitly. A scan's `real_time` is per traversal;
derived time per key divides by its record count. Repetition statistics are
not per-request P95/P99 latency.

Measured operations still fail on storage errors, incorrect presence/absence,
or unexpected iterator cardinality. Full content/order verification also runs
outside timing. The benchmark does not turn a failed read into a fast result.

### Build and result contract

Add a `profiling` preset with Release optimization, debug symbols, frame
pointers, and compile commands for the engine, dependencies, and harness.
Do not use `RelWithDebInfo`'s potentially different optimization level as a
stand-in for Release. Do not disable inlining in the production library.

A custom main calls Google Benchmark initialization/reporting directly and
does **not** invoke `BENCHMARK_MAIN` or `MaybeReenterWithoutASLR`. The tool
must not change process ASLR or host security settings for measurement.

Keep Google Benchmark JSON unmodified and add a versioned sidecar containing
the case, effective options, data/query digests, source revision/dirty state,
compiler/flags, dependency provenance, executable digest, tool versions, and
actual command argument arrays. A source override or dirty tree is recorded
explicitly. Uncommitted-source measurements are diagnostic results, not a
claim of reproducibility from a commit alone.

The runner validates individual rows with `run_type=iteration`, not aggregate
rows. It requires the selected case, expected repetition count, one foreground
thread, positive iteration/item counts, finite positive wall time,
nonnegative finite CPU time, and no error or skip flags. Zero CPU time is
allowed for very short smoke runs because of timer resolution.

The executable fails on a benchmark error or no matching case, and the
runner independently validates the report. An empty result, skipped case,
malformed JSON, or incomplete output is never successful just because a
process returned zero.

Do not translate this new report into the legacy benchmark schema or silently
replace its gate. The first CI checks correctness, report shape, and case
coverage, not a new noisy throughput threshold.

### CPU profile capture

Automated CPU capture initially targets **macOS Apple Clang with Xcode Time
Profiler**. Linux builds and runs the same benchmark cases, but an automatic
`perf` collector is deferred until it can be validated on a suitable host.
This is an explicit scope boundary, not a hidden fallback.

Use `xctrace record --template 'Time Profiler' --launch` for one owned
benchmark process. Use `dsymutil` for the profiled executable. Never attach
to all processes or select a process by a partial name.

In profile mode only, a small benchmark-local scope emits a Points of
Interest signpost interval around each benchmark loop, after fixture setup
and warmup. The interval carries a case identifier and planned/completed
iteration counts. No markers or timers are added to `src/` or public APIs.

The collector requests one case, one repetition, no framework warmup, and
a bounded minimum run time. Adaptive calibration may emit several intervals.
Select the final complete interval whose case and iteration count match the
reported individual run. Exclude all other intervals and all preparation
samples.

Export the trace table of contents, all `os-signpost` tables, and
`time-profile` samples. Resolve XML `id`/`ref` references across the export,
read raw typed values rather than localized `fmt` attributes, and deduplicate
identical events. Do not assume the first matching signpost table has rows.

Retain the raw trace and derive a small CPU sample report for the selected
PID and interval: sample count, self/inclusive weights, unresolved-symbol
count, and per-thread attribution. Inclusive percentages overlap and must not
be added together. Empty or unmatched windows fail; small sample counts are
explicitly marked low-confidence rather than promoted to precise CPU times.

Check collector status, target exit status in the trace, complete benchmark
JSON, paired boundaries, and nonempty in-window samples. A timed-out,
killed, or failed target is not a successful profile. The verified xctrace
launch mode ends when the target exits and terminates it at the recording
limit.

The collector is started in its own process group. Research established that
its launched target is a direct child **in a different process group**, so
killing only the collector group is not a cleanup protocol. Discover only
the collector's direct children, verify the target's executable/parent and
process identity, and retain that identity while the collector is alive.
Do not use buffered redirected stdout as the process-start notification.

On the outer deadline or cancellation, stop the verified target explicitly,
then request collector termination and finalization. After bounded grace
periods, escalate signals only for the verified target and the runner-owned
collector group. Reap the direct collector and confirm the target has exited
before deleting `work/`. Revalidate identity before signaling; if termination
cannot be proved, fail explicitly and retain the scratch directory rather
than deleting files beneath a live process or signaling an unrelated PID.
The workload does not launch grandchildren.

Capture has a finite outer deadline and preserves diagnostic output on
failure; it never deletes or overwrites an existing artifact directory.

`sample` remains useful for manual wall-stack investigation but is not an
automatic substitute for CPU Time Profiler data. Allocation, lock, I/O,
PerfContext-style counters, and Linux perf automation are separate follow-up
work selected by evidence.

Native traces can contain local paths and device information. Keep them under
ignored build output and do not upload them automatically. CI uploads only
its synthetic benchmark reports and logs.

### Optimization admission

The infrastructure PR changes no engine algorithm, storage format, public
options, or snapshot/concurrency guarantee. A later optimization names:

- the measured scenario and repeatable bottleneck;
- the relevant mature strategy and why its prerequisites hold here;
- the expected effect and an unprofiled before/after comparison;
- correctness and non-target regression checks.

Do not use profiler-instrumented timing as the claimed speedup. Do not copy
RocksDB cache capacities, full/Ribbon formats, parallel writers, or compaction
policies without a separate decision and workload evidence.

## Acceptance and implementation order

The bounded file plan, negative cases, collector checks, and research receipts
are in [the implementation plan](../profiling-design.md). Complete one coherent
PR. If a new result contradicts a recorded premise, stop and revise the
decision explicitly rather than layering workarounds into implementation.

## References

- [Google Benchmark v1.9.5 guide](https://github.com/google/benchmark/blob/v1.9.5/docs/user_guide.md)
- [Benchmark runner and calibration](https://github.com/google/benchmark/blob/v1.9.5/src/benchmark_runner.cc)
- [Timing and ASLR entry point](https://github.com/google/benchmark/blob/v1.9.5/src/benchmark.cc)
- [JSON reporter](https://github.com/google/benchmark/blob/v1.9.5/src/json_reporter.cc)
- [Pinned LevelDB workloads](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/benchmarks/db_bench.cc)
- [RocksDB profiling guidance](https://github.com/facebook/rocksdb/wiki/Perf-Context-and-IO-Stats-Context)
