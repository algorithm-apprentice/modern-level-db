# Profiling Research and Implementation Plan

## Purpose and boundary

This is the implementation handoff for [ADR-0042](adr/0042-benchmark-profiling-foundation.md).
It resolves tool and measurement behavior before the infrastructure is built.
It is not an optimization patch, a new benchmark framework, or a replacement
for ADR-0041's existing regression gate.

The first deliverable is a Google Benchmark-based read-workload harness,
strict result collection, and a macOS CPU profile with an explicit measured
window. Automatic Linux perf capture, tail-latency load testing, production
statistics, and engine changes are outside this slice.

## Decisions backed by primary sources and probes

Research used Google Benchmark v1.9.5 at
`192ef10025eb2c4cdd392bc502f0c852196baa48`, the repository's pinned LevelDB
reference, RocksDB v11.8.1 interfaces, official profiling documentation, and
small isolated programs outside production source.

| Question | Established behavior | Design consequence |
|---|---|---|
| Which framework? | Google Benchmark already provides calibration, repetitions, CPU/wall timers, counters, selection, and JSON | Reuse it; keep custom code to workloads, adapters, collection, and validation |
| Is the pinned version buildable? | Isolated optimized/debug-symbol builds succeeded with Apple Clang and GCC 16 | Pin that source; additionally validate GCC 13 in the planned Linux acceptance job |
| Can setup repeat? | Adaptive minimum-time execution called the probe six times; `100000x` with three repetitions called it exactly three times | Prepare the selected fixture once per process; do not recreate a DB on calibration attempts |
| Is setup timed? | A 50 ms sleep before the state loop did not enter its per-iteration timings | Populate, reopen, verify, and warm outside the loop |
| Does default CPU time include workers? | A sleep-loop probe with a busy background thread reported about 4 us/iteration of thread CPU, but about 1.23 ms/iteration with process CPU | Use process CPU plus wall time; retain per-thread profile attribution |
| Are scan and Get iterations interchangeable? | Native JSON is per benchmark iteration; work counters are separate | Report explicit items per iteration and normalize scans per key only in derived output |
| Can exit zero mean failure? | Both `SkipWithError` and a filter matching no cases returned zero in the stock-style probe | Custom main plus strict JSON validation must reject both |
| Can aggregate rows contain zeros? | Standard deviation/CV rows contained zero counter values despite valid individual runs | Validate `iteration` rows separately and preserve aggregates without misreading them |
| Does the usual main change ASLR? | `BENCHMARK_MAIN` calls `MaybeReenterWithoutASLR`; on Linux that helper attempts `ADDR_NO_RANDOMIZE` and re-exec | Use a custom main and do not call that helper |
| Are symbols available under optimization? | Compile commands contained `-O3`, `-DNDEBUG`, `-g`, and frame pointers; Time Profiler resolved the probe's named frames | Preserve Release behavior and add symbols, rather than profiling Debug |
| Is CPU capture actually usable? | Xcode Time Profiler launched and profiled an owned optimized process without interactive prompts | Use this verified capture path for the first automatic collector |
| Can preparation be separated? | A one-second preparation and seven-second marked workload produced 7,000 in-window measured samples and zero preparation-marker samples | Select by signpost interval; never infer a window from guessed delays |
| Does calibration confuse the window? | A profiled benchmark emitted seven calibration/measurement intervals; the last interval's 992,453 iterations matched its JSON result | Match the complete interval to the case and reported iteration count |
| Is display text machine-readable? | An exported display value was `992,453`, while the typed `uint64` text was `992453` | Parse raw typed values, not `fmt` text |
| Is there one populated marker table? | The first signpost table was empty; later tables repeated events using cross-table references | Export all relevant tables, resolve references, and deduplicate |
| What happens on failure/timeout? | An intentional target exit 7 and a recording limit both returned collector status 54; the trace recorded exit 7 or SIGKILL respectively, with no surviving limited probe | Fail the capture; verify target status and do not accept an incomplete report |
| Will killing the collector group stop the target? | A session-isolated collector launched its own direct child in a different process group; explicitly killing that verified child produced status 54/SIGKILL, no surviving target, and successful scratch cleanup | Track the verified child separately; do not assume a common process group |
| Is redirected stdout a readiness signal? | Target stdout was buffered by capture: waiting for its PID text could miss the live workload | Discover only the collector's direct children and validate their executable and process identity |
| Is corpus generation portable? | The exact generator below produced identical fingerprints with Apple Clang/libc++ and GCC 16/libstdc++ | Freeze its byte encoding, draw schedule, rejection rule, and insertion order |

Probe inputs and generated traces were kept in ignored local research output.
The table records behavior, not timing guarantees for a different machine.
Raw native traces contain machine-specific information and are not committed.

## First workload family

Use one selected case per process. There are sixteen supported combinations:

| Workload | Operation under measurement | Checks |
|---|---|---|
| `readrandom` | Get an existing key from a precomputed uniform order | Successful presence; no storage error |
| `readmissing` | Get an interleaved absent key | NotFound/empty result, not a fabricated empty value |
| `scan` | Create iterator, seek first, traverse, check, and destroy iterator on every iteration | Exact record count; no iterator error |
| `seek_reuse` | Seek one retained iterator to existing keys | Valid position at the expected key; no iterator error |

Run each against both implementations with 4,096 and 65,536 records.
Full content/order checks before and after measurement guard dataset and
driver correctness without building a correctness framework into every timed
operation. Main runs the post-measurement check exactly once after
`RunSpecifiedBenchmarks()` returns, not after every adaptive callback and not
inside a destructor.

The first matrix is deliberately read-only and single-foreground-thread.
The last comparison showed read/scan gaps, not a write-throughput problem.
This does not establish cold-device I/O, mixed-load, concurrent-write, or
P99 behavior. Add those only as named follow-up scenarios.

### Canonical corpus and orders

For `N` equal to 4,096 or 65,536:

1. Record `i`, for `0 <= i < N`, has key ASCII `"k"` followed by **exactly ten
   decimal digits** encoding `2*i`, zero-padded on the left.
2. It has 256 value bytes. If `i % 4 != 0`, all bytes equal ASCII
   `'a' + (i % 26)`. Otherwise, seed one `std::mt19937_64` with 301 once and
   consume exactly 256 draws for that record, taking the low eight bits of
   each draw. Repetitive records consume no data-generator draws.
3. Missing key `i`, for `0 <= i < N-1`, uses the same key encoding with
   `2*i+1`. Every absent key is therefore strictly inside the dataset's range.
4. Start each order as `[0, 1, ..., count-1]`. Use a separate `mt19937_64`
   initialized with seed 302 for `N` insertion indices, seed 303 for `N`
   present query indices, and seed 304 for `N-1` missing query indices.
5. Shuffle each order as follows. All arithmetic shown for random draws,
   `bound`, and `threshold` is unsigned 64-bit arithmetic:

```text
for size = count down to 2:
    bound = uint64(size)
    threshold = (uint64(0) - bound) % bound
    do draw = generator() while draw < threshold
    j = draw % bound
    swap(order[size - 1], order[j])
```

Only the three resulting permutations drive insertion and point/seek queries.
Queries wrap at their order's length; reset the cursor to zero before each
measured callback. This fixes workload semantics across standard libraries
without requiring identical compaction scheduling or SST layout.

Fingerprints use the existing unmasked CRC32C implementation, initially zero,
over the following byte streams. `LE64` means eight little-endian bytes, and
ASCII domain strings have no trailing NUL:

```text
records:
  "modern-perf-records-v1" || LE64(N) ||
  for i in [0, N):
    LE64(key_length) || key_bytes || LE64(256) || value_bytes
insertion / present / missing:
  respective domain || LE64(order_length) || LE64(each index in order)
domains:
  "modern-perf-insert-v1"
  "modern-perf-present-v1"
  "modern-perf-missing-v1"
```

Both tested compiler/standard-library pairs produced:

| Records | Record CRC32C | Insertion CRC32C | Present-order CRC32C | Missing-order CRC32C |
|---:|---|---|---|---|
| 4,096 | `e966aa2f` | `387c287f` | `c3e3b3de` | `7883c9b4` |
| 65,536 | `3fbabb34` | `347ed266` | `2422abad` | `8ee790ec` |

These are drift-detection fingerprints, not collision-resistant signatures.
The artifact additionally carries a SHA-256 digest of the executable.

### Cache and verification lifecycle

The runner owns the scratch directory. The custom main owns the lazy fixture
and registered callback. The sequence is:

```text
validate arguments and case
on first callback only:
  generate the canonical corpus and orders
  insert in canonical shuffled order into the runner-owned fresh path
  close and reopen
  full ordered content verification, fill_cache=false
  one complete selected-workload warmup pass, fill_cache=true
before each callback loop:
  reset query cursor
  for seek_reuse, rewind the retained iterator outside timing
  emit Begin in capture mode
measured loop:
  execute the selected operation, fill_cache=true
  for scan, create and destroy its iterator inside each iteration
after each callback loop:
  emit End in capture mode; publish benchmark counters
once RunSpecifiedBenchmarks returns:
  release retained query iterator
  full ordered content verification, fill_cache=false
  explicitly destroy the database; propagate verification failures
after process termination:
  runner removes only its work/ scratch directory; preserves artifacts
```

Cache contents carry forward through calibration and repetitions; neither
cache nor page cache is reset. Calling `fill_cache=false` avoids inserting
uncached blocks but does not prevent the engine from reading a block already
in cache. Automatic compactions may still run, and their CPU activity is
part of the process-wide profile rather than an unproven quiescence assumption.

### Avoiding misleading comparisons

- Match storage options, including the 8 MiB block-cache budget, WAL,
  checksum policy, and codec build.
- Do not compare our 8 MiB default with RocksDB's documented 32 MiB default.
- Negative keys must fall between valid key positions.
- Do not create a new iterator for `seek_reuse`; that would measure a different
  operation. Do not label it an exact clone of upstream `seekrandom`.
- Keep fixture generation and warmup out of both reported timing and the
  selected CPU window.
- Do not assume automatic compactions have stopped; process CPU and separate
  background-stack attribution expose their activity.
- Use the natural output ownership of each public API. Do not introduce a
  reference-only copy to hide Modern LevelDB's current return-value allocation.
- Do not infer gains from a single timing run or from a profiler's own timings.

## Implementation map

The implementation follows the file and interface boundaries established
before development:

| Order | Surface | Work |
|---|---|---|
| 1 | `cmake/GoogleBenchmark.cmake`, root CMake and presets | Pinned optional dependency, explicit collision checks, shared LevelDB reference, Release-symbol `profiling` preset |
| 2 | `benchmarks/profiling_bench.cc`, `profiling_build.h.in` | One-case selection, two typed adapters, deterministic fixture/query generation, four workloads, natural ownership, process CPU/wall counters, strict main and build provenance |
| 3 | Same benchmark translation unit | Profile-only macOS signpost scope around the loop; named optimized workload frames; no production markers |
| 4 | `tools/run_performance.py` | Case validation, benchmark invocation, fresh artifact directory, provenance, raw report validation, finite deadlines |
| 5 | `tools/profile_report.py` | Time Profiler launch/export, target status checks, typed XML references, deduplication, measured-window selection, CPU sample summary |
| 6 | `tests/tools/` and optional CMake checks | Predefined positive/negative contract tests, sixteen-case smoke matrix, default-off/parent-isolation checks |
| 7 | CI and user documentation | Linux GCC 13 and macOS benchmark smoke; local real capture evidence; retain all existing gates |

Keep these changes in one infrastructure PR. Do not extract or rewrite legacy
benchmark helpers merely for aesthetic deduplication. Do not add public cache
configuration or statistics APIs as a prerequisite for this slice.

### Artifact contract

An ordinary run produces raw Google Benchmark JSON, process logs, and a
versioned manifest. A CPU capture additionally produces the raw `.trace`,
table of contents, signpost/sample exports, and a small summary.

The manifest records:

- selected case and exact operation/item units;
- effective engine options and data/query digests;
- framework/reference/codec provenance, including explicit source overrides;
- compiler, optimization/symbol flags, executable digest;
- source base revision, runtime revision, and dirty-state information;
- collector version, argv arrays, exit statuses, and artifact-relative paths.

Never overwrite an existing output directory. Keep failures visible and retain
their logs. Do not store native host/device identifiers in the derived summary
or automatically upload raw local traces.

### Measurement defaults

The benchmark runner uses three repetitions, calibrating the first
against a requested minimum of 0.2 seconds of measured wall time. Google
Benchmark reuses the calibrated iteration count for later repetitions; those
repetitions are **not guaranteed** to run for 0.2 seconds. The smoke runner
uses one explicit iteration per case and validates behavior/output, not speed.

The CPU collector uses one repetition, five seconds of measured wall
time, no Google Benchmark warmup phase, and a 120-second recording limit.
The fixture still performs its explicit warmup. The outer collector deadline
is 180 seconds, allowing finalization after the recording limit. An ordinary
benchmark run has a 300-second outer deadline.

Create the fresh output directory before launching any process, with a
separate runner-owned `work/` subtree and a nonexistent `work/db` target.
On normal completion, verify the process/report then remove `work/`. On a
recording or outer timeout, terminate the verified workload PID first, allow
five seconds for collector shutdown, then request collector-group SIGINT and
allow ten seconds for finalization before SIGKILL and reaping. The target's
identity must be rechecked before escalation. Stop/delete nothing by a
partial process name. If the target cannot be proven stopped, preserve its
scratch path and fail loudly instead of claiming cleanup.

The native probe established that the xctrace target is a direct child in its
own process group. Accordingly the collector must track both, not assume
`killpg(collector)` is enough. PID discovery uses direct-child inspection and
exact executable/parent validation, not the potentially buffered
`--target-stdout` file. No broad process scan or all-process trace is required.

These are collection budgets, not pass/fail performance thresholds. Small CPU
sample counts are reported as low-confidence; zero valid in-window samples or
missing boundaries are errors.

## Running the infrastructure

The opt-in preset uses Release optimization with symbols and frame pointers.
The normal build and the older comparative benchmark remain unchanged:

```bash
cmake --preset profiling
cmake --build --preset profiling
ctest --preset profiling
build/profiling/benchmarks/modern_leveldb_performance --list-cases
```

Run one case without a profiler for timing evidence:

```bash
python3 tools/run_performance.py \
  --binary build/profiling/benchmarks/modern_leveldb_performance \
  --case modern/readrandom/65536 \
  --output build/performance/modern-readrandom-64k
```

Select the same case with `--capture-cpu` for CPU attribution. This needs a
macOS Apple Clang build and Xcode's `xctrace`/`dsymutil`:

```bash
python3 tools/run_performance.py \
  --binary build/profiling/benchmarks/modern_leveldb_performance \
  --case modern/readrandom/65536 \
  --capture-cpu \
  --output build/performance/modern-readrandom-64k-cpu
```

Output paths must be new. The runner preserves raw results and diagnostics,
removes only its stopped process's `work/` scratch directory, and leaves
unverifiable cleanup as an explicit failure. `--smoke` runs one iteration
without making a performance claim. `--min-time`, `--repetitions`, and
`--timeout` accept explicit collection budgets; CPU capture requires one
repetition and does not permit smoke mode.

Use `manifest.json` for provenance and normalized per-item times,
`benchmark.json` for untouched Google Benchmark output, and `completion.json`
for corpus/lifecycle assertions. A CPU capture additionally has
`profile-summary.json` (weighted self/inclusive stacks and thread attribution),
the executable/dSYM snapshot, and raw trace/export files.
Every command journal entry includes its artifact-relative log path, and the
manifest indexes the diagnostic logs. CPU manifests also record the parsed
xctrace version and build rather than leaving that provenance only in console
output.

Raw traces and compiler command paths stay local by default. Do not upload
them from a developer machine without reviewing their contents.

## Fixed acceptance checklist

### Framework and workload contract

1. The normal build with the option off acquires no Google Benchmark target
   or download and retains existing behavior.
2. The optional target builds with the pinned API on Linux GCC 13 and macOS
   Apple Clang; the profiling flags preserve Release optimization.
3. Help/listing/invalid case selections do not create databases.
4. A fixture is prepared once even when adaptive calibration invokes its
   callback repeatedly, and final verification runs only once in main.
5. Each of the sixteen cases executes and reports the correct engine,
   record count, operation/item unit, and one foreground thread.
6. Dataset, absent-key, scan cardinality, and reused-iterator semantics are
   verified against the canonical fingerprints and insertion/query sequences.
   Scan creation/rewind/destruction boundaries are checked. Unsupported
   operations are rejected, not stubbed with success.
7. Setup and teardown are excluded; process CPU accounts for a controlled
   worker thread; scan normalization is tested independently.

### Result and failure contract

1. Reject no matches, zero individual rows, skip/error flags, malformed or
   partial JSON, wrong case/repetition counts, and inconsistent iteration/item
   counters.
2. Reject non-finite or negative timings, allowing zero CPU time only as
   timer resolution permits; require positive wall time.
3. Do not mistake mean/median/stddev/CV aggregate rows for individual runs.
4. A deliberate operation failure causes a failing executable and runner
   result, despite Google Benchmark's stock exit behavior.
5. Existing artifact directories are not overwritten and failed runs preserve
   diagnostic output.

### Profile contract

1. Capture a real owned process and resolve the named measured workload frame.
2. Confirm target exit status zero and a complete benchmark result.
3. Resolve reference-only XML elements and repeated table events; handle empty
   tables and display values containing thousands separators.
4. Select exactly the final complete case/iteration-matching window, not an
   earlier calibration or preparation interval.
5. Reject missing/duplicate-conflicting boundaries, missing references,
   failed/killed targets, empty samples, and unsupported collector platforms.
6. A preparation/workload control probe proves preparation samples are
   excluded. A timeout control proves the recording cannot become a false
   success and that neither the owned process nor its scratch database
   survives successful cleanup; unverifiable cleanup is a reported failure.
7. Collect the two engines' read/scan baselines locally, but make optimization
   claims only from separate unprofiled runs.

## How RocksDB research will be used afterward

| Observed bottleneck | Mature strategy to evaluate | Prerequisite |
|---|---|---|
| Allocation/copy CPU | Compact cache entries, buffer reuse, PinnableSlice-style ownership | Preserve RAII and demonstrate the allocation/copy cost |
| Cache pressure/contention | Capacity/sharding policy and cache layout | Measure hit/miss and contention; more shards are not automatically better |
| Device wait dominates | Bounded readahead or asynchronous I/O | Actual storage-bound workload and supported filesystem primitives |
| Write amplification/debt | Dynamic level targets or subcompactions | Sustained multi-level workload; redesign scheduling invariants explicitly |
| Need finer operation attribution | PerfContext/IOStatsContext-style thread-local counters | Sampling cannot answer the question; keep timing opt-in |

Full/Ribbon filters and block hash indexes need a storage-format/metadata
decision. Concurrent memtable writes require a concurrent-writer structure,
not a configuration flip. Unordered writes relax snapshot immutability and
are not an admissible speed shortcut under the current API contract.

Our index and filter objects already live with the table cache, outside the
data block cache. RocksDB metadata-priority policies therefore do not
automatically improve this implementation.

## Research closure

The bounded GPT-5.6 Sol design review identified six missing contracts. All
were resolved before implementation: exact corpus/insertion generation,
cache initialization, one-shot final verification, scan iterator lifetime,
outer-timeout ownership/cleanup, and the actual per-repetition timing rule.
The suggested common-process-group cleanup was independently checked rather
than adopted blindly; the verified design tracks the target separately.

At design freeze, no unresolved architectural or tool-behavior question
remained inside the initial scope. GCC 13 integration, full fixture execution,
and real engine profiles were retained as explicit implementation acceptance
work, not claimed as completed by the design document.
Automatic Linux perf collection and other deferred mechanisms are not
silently scheduled for discovery during implementation.

If acceptance contradicts a premise, record the evidence and amend the
decision before changing approach; do not weaken checks or accumulate
unexplained patches. Regression checks remain mandatory: completing research
does not mean an implementation no longer needs validation.
