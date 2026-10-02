# Write Profiling Research and Implementation Handoff

This document implements the decisions in
[ADR-0045](adr/0045-fixed-work-write-profiling.md). It extends the existing
profiling harness, not the engine or its public API.

## Running the fixed-work cases

Build the existing `profiling` preset, then select one case per process:

```bash
python3 tools/run_performance.py \
  --binary build/profiling/benchmarks/modern_leveldb_performance \
  --case modern/overwrite/65536 \
  --output build/performance/modern-overwrite-run-1
```

The other workload identifiers are `writebatch/65536`, `writesync/4096`, and
`mixed50/65536`; each supports both engine prefixes. The normal budgets are
262,144 single writes, 8,192 batches of 32, 1,024 sync writes, or 262,144
read/write pairs, respectively. Use `--smoke` for one iteration with unchanged
preparation; it is not a performance sample.

Do not pass `--min-time` or request multiple repetitions. To compare engines
or revisions, collect independent invocations with new output directories in
a predetermined alternating order. Do not aggregate samples from different
budgets. `items_per_iteration` is 1, 32, or 2: mixed per-item time averages a
read and a write, and batch per-item time is per submitted key.

On macOS AppleClang builds, `--capture-cpu` captures the same normal fixed work.
Inspect `background_activity.quarters` in `profile-summary.json` for sampled
non-foreground compaction and flush activity. A false
`sustained_compaction_observed` means the evidence was not observed, not that
there was no compaction. A true value still does not establish steady state
or completion of outstanding background work.

Run the no-database operation-stream diagnostic directly with:

```bash
build/profiling/benchmarks/modern_leveldb_performance --check-mutation-stream
```

`ctest --preset profiling` retains the original contracts, runs all 24 cases
in smoke mode, checks the recording diagnostic, and executes all eight normal
mutable cases. Its raw reports are under `build/profiling/benchmarks/`
in `performance-smoke/` and `performance-mutations/`.

## ADR-0060 write-path parity extension

The final write-path parity measurement keeps the fixed operation streams and
adds explicit benchmark roles. The executable accepts:

```text
--modern-write-batch-ownership copying|exclusive
--modern-wal-creation durable|leveldb
```

Defaults remain `copying` and `durable`. An explicitly selected batch role is
valid only for `modern/writebatch/65536`; `exclusive` calls
`Database::WriteExclusive`, while `copying` retains `Database::Write`.
An explicitly selected WAL role is valid only for Modern mutable cases;
`leveldb` sets `sync_wal_creation=false`, while `durable` retains the
production default. LevelDB and unrelated workloads report both roles as
`not_applicable`. Invalid values, duplicate selectors, and invalid
case/selector combinations fail before opening the database.

Every Google Benchmark context and retained manifest records:

```text
modern_write_batch_ownership
modern_write_batch_ownership_semantics
modern_wal_creation
modern_wal_creation_semantics
reference_hardware_crc
```

The versioned semantics are:

```text
copying       -> const-copy-v1
exclusive     -> exclusive-borrow-v1
durable       -> file-and-directory-before-write-v1
leveldb       -> pinned-leveldb-v1
not_applicable
```

The hardware-CRC marker is build provenance, not a runtime selector. Canonical
builds report `disabled`; the retained measurement-only LevelDB link patch
reports its exact versioned marker. `tools/run_write_parity.py` compares that
marker and the complete expected frozen-build identity supplied by its plan.

### Schema-3 mutable completion

Mutable completion reports advance from schema 2 to schema 3. All schema-2
fields remain unchanged. After the final verified close, the benchmark scans
the database directory and adds these nonnegative integer fields:

```text
residual_wal_files
residual_wal_bytes
residual_table_files
residual_table_bytes
residual_manifest_files
residual_manifest_bytes
residual_regular_files
residual_regular_bytes
```

WAL files end in `.log`; table files end in `.ldb` or `.sst`; MANIFEST files
begin with `MANIFEST-`. Total residual metrics include every regular file in
the database directory, including metadata files. Each category must fit
within the total, and the sum of the three named categories must not exceed
the total. These values are descriptive; the harness does not force
background completion or infer steady state from them.

`tools/run_performance.py` retains the existing normalized values and also
stores the raw Google Benchmark per-iteration values:

```text
wall_ns_per_iteration
process_cpu_ns_per_iteration
```

For fixed mutable cases each array has exactly one value. Matrix aggregation
uses these per-iteration arrays as its primary input. `writebatch` additionally
derives per-written-key values by dividing the batch-call value by 32; it never
substitutes normalized per-item input for the primary metric.

### Predeclared parity plan

`tools/run_write_parity.py` accepts only:

```text
--plan PLAN.json
--output NEW_DIRECTORY
```

The plan has `schema_version=1`, `matrix=write-path-parity-v1`, and exactly
three frozen binary roles: `final`, `baseline`, and `canonical`. Each role
contains an absolute executable path, expected SHA-256, expected compile
commands SHA-256, an exact expected build-provenance object, and an exact
runtime source-state object. The plan also records the already completed
correctness/review evidence for the final revision. Required build identity
includes source/build directories, configure revision and dirty state,
compiler and complete flags, pinned dependency revisions, the hardware-CRC
marker, CRC provider/source/capabilities, and reference control patch hash.

The top-level plan contains exactly `schema_version`, `matrix`, `roles`, and
`evidence`. Every role contains exactly `executable`, `executable_sha256`,
`compile_commands_sha256`, `build`, `runtime_source`, and `reference`.
`runtime_source` must be the available clean/dirty source-state object emitted by
`run_performance.py`, including both its porcelain-status SHA-256 and a
SHA-256 over the tracked binary diff plus every untracked path and file
content. Frozen matrix roles must have zero untracked files. The driver retains
each role's exact binary `git diff` and its SHA-256 in the matrix output.

The exact `reference` proof contains absolute paths and SHA-256 values for the
generated LevelDB `port_config.h` and `libleveldb` archive, the expected
Boolean `HAVE_CRC32C` state, and either the exact hardware-control patch
SHA-256 or `not_applicable`. Before every process, the driver rehashes these
files, parses the header, finds LevelDB's `util/crc32c.cc` command in the
retained compile database, and inspects the archive's undefined symbols. The
hardware role must compile with the pinned Google CRC32C include directory and
reference `crc32c::Extend`; canonical and baseline roles must do neither. The
symbol check accepts both GNU `nm -u -C` output with a `U` type prefix and
Darwin output that prints only the demangled undefined symbol. It requires the
top-level Google namespace and must not treat LevelDB's portable
`leveldb::crc32c::Extend` symbol as hardware-provider evidence.

`evidence` contains `final_revision`, a `hardware_crc_profile` object, plus Boolean
`correctness`, `compatibility`, `crash`, `sanitizers`, `compilers`, `coverage`,
`benchmark_contracts`, and `review` gates; every gate must be true before a
matrix can start, and `final_revision` must equal the final role's configure
and runtime revision. The profile object identifies retained profile manifest
and summary files by absolute path and SHA-256. They must describe the final
binary, exact final build identity, `leveldb/readrandom/65536`, at least 100
samples without a low-confidence warning, and a positive
`crc32c::ExtendArm64` inclusive sample weight.

The three source directories, build directories, and executable paths must be
distinct. The final role must declare a non-disabled versioned hardware-CRC
marker, pinned-source provider, empty CRC source override, compiled ARM64
capability, `HAVE_CRC32C=1`, and an exact retained hardware patch. The
canonical role must declare `disabled`, `HAVE_CRC32C=0`, and no hardware
patch. Final and canonical use the same final source revision. Baseline uses a
different pre-parity revision. All three roles must use the same generator,
target architecture, compiler, complete flags, requested dependency revisions,
reference-control patch, and no dependency source overrides. The normalized
Modern `write_path.cc` compile command must also match between baseline and
final. Final and canonical executables must have different SHA-256 values.

The driver hashes each binary before every process and validates every
completed `run_performance.py` manifest against the selected role. A changed
binary, mixed revision, changed dirty-state fingerprint, missing compile
commands, unexpected role marker, or inconsistent build identity fails the
entire matrix. The output directory must not exist; failed matrices are
retained but cannot be resumed, amended, or used to replace individual cells.

The fixed execution order is matrix, round, ADR workload order, then the two
adjacent pair members. Odd rounds run the reference/control first; even rounds
run the candidate first:

| Matrix | Rounds | Pair | Processes |
|---|---:|---|---:|
| `primary` | 5 | hardware LevelDB / matched Modern | 40 |
| `production` | 5 | pre-parity Modern / final production Modern | 40 |
| `batch_ownership` | 3 | exclusive / copying Modern `writebatch` | 6 |
| `crc_continuity` | 3 | canonical / hardware LevelDB | 24 |

The exact total is 110 fresh processes. The driver has no workload, round,
repetition, calibration, minimum-time, cell-selection, or resume options.
Every fixed-work report must contain one callback, one framework repetition,
the exact operation count, schema-3 completion, required residual metrics, and
the expected role provenance.

Aggregation rejects duplicate or missing cells and compares the canonical
completion fingerprint and operation counts across every role for a workload.
It reports per-round and median wall/process-CPU values from raw per-iteration
timings, primary and production deltas, the descriptive batch and CRC controls,
and the non-`writebatch` production/matched WAL-durability ratio.

Performance deltas are diagnostic and have no preset pass/fail threshold.
Collection succeeds only when completion, residual, frozen identity, and
correctness/review evidence are valid. Positive deltas remain regressions and
must be reported and analyzed rather than relabeled, hidden, or replaced. No
failed cell can be discarded or replaced.

## Resolved research

The isolated native probe linked the existing pinned Google Benchmark,
LevelDB, codecs, CRC32C, and Modern LevelDB libraries. Eight successful runs
checked all final values both live and after reopen. Even when passed
`--benchmark_repetitions=7 --benchmark_min_time=999x`, registration's explicit
iterations and one repetition produced exactly one callback with the intended
work budget. A conflicting dry-run failed before opening a database; framework
warmup failed rather than yielding an accepted second callback.

| Engine | Overwrite loop | Batch loop | Sync loop | Mixed loop |
|---|---:|---:|---:|---:|
| Modern | 20.93 s | 17.51 s | 3.93 s | 23.05 s |
| LevelDB | 30.86 s | 23.58 s | 3.96 s | 33.91 s |

These are budget probes, not performance claims or paired comparisons.
Preparation ranged from about 0.4 to 14.1 seconds. Two further native
overwrite captures completed within the unchanged collector limits:

| Engine | In-window samples | Foreground samples | Compaction samples by quarter | Flush samples by quarter |
|---|---:|---:|---|---|
| Modern | 10,361 | 1,379 | 2,105 / 2,094 / 2,125 / 2,090 | 23 / 23 / 19 / 31 |
| LevelDB | 10,887 | 1,200 | 2,411 / 2,420 / 2,387 / 2,366 | 5 / 7 / 7 / 6 |

All selected intervals had zero calibration intervals. The counts demonstrate
sampled ongoing flush/compaction, not identical compaction debt or a steady
state. Raw probes and native captures are ignored local artifacts.

## Implementation boundaries

| Surface | Change |
|---|---|
| `benchmarks/profiling_bench.cc` | Extend explicit case enumeration; reuse corpus, adapters, markers, and reporter; add a separate mutable fixture so read lifecycle/timing stays unchanged |
| Same file, adapters | Add sync Put, batch, and full-value read operations used only by mutable cases; no new persistent state in read adapters |
| Same file, mutable fixture | Precompute versions/order, track current generations and successful counts, run one fixed loop, verify live and reopened state, emit schema 2 |
| Same file, benchmark-only diagnostic | `--check-mutation-stream` drives the shared per-iteration dispatcher with an independently checking recording adapter; no database or production instrumentation |
| `tools/run_performance.py` | Enumerate the eight new cases, fixed-mode defaults and flag rejection, strict result/completion cross-checks, existing artifact ownership/cleanup |
| `tools/profile_report.py` | Recognize new loop symbols and derive sampled background-quarter evidence for mutable captures |
| `tests/tools/` | Synthetic schema/trace failures, executable lifecycle/flag tests, all-case smoke and full mutable matrices |
| `benchmarks/CMakeLists.txt`, README, profiling guide | Register bounded matrix checks and document new commands/units/limits |

Do not refactor unrelated benchmark helpers, change the read operation bodies,
add a general workload DSL, or create public engine instrumentation.

## Canonical mutation fingerprints

All fingerprints are unmasked CRC32C over the stated byte stream, initially
zero. `LE64`, length-prefixed `Field`, and original corpus fingerprints retain
ADR-0042's definitions. These fingerprints detect corpus drift; executable
identity still uses SHA-256.

```text
write_order:
  ASCII("modern-perf-write-order-v1") || LE64(N) ||
  LE64(each index in the seed-305 write permutation)

version_values:
  ASCII("modern-perf-write-values-v1") || LE64(N) || LE64(5) ||
  for key index i in increasing order:
    Field(key_i) ||
    for generation g in [1, 5]:
      LE64(g) || Field(value_i_with_first_8_bytes_replaced_by_LE64(g))

final:
  ASCII("modern-perf-write-final-v1") || LE64(N) ||
  for actual records in the final reopened iterator:
    Field(key) || Field(value)
```

The reopened scan must also compare every actual key/value and cardinality
against the successful-write model, not accept a CRC collision as correctness.
An independent bit-at-a-time CRC oracle produced identical results under
AppleClang/libc++ and GCC 16/libstdc++:

| N | Original records | Write order | Version values | Normal final | One-write smoke final | 32-write smoke final |
|---:|---|---|---|---|---|---|
| 4,096 | `e966aa2f` | `f117174a` | `204ed629` | `5ff7de22` | `92030b01` | Not a supported case |
| 65,536 | `3fbabb34` | `365dce99` | `c93270ce` | `b9ae033b` | `7dc2dbe1` | `86c2c994` |

Normal final means the first 1,024 write-order keys at generation 2 for
4,096-record sync, or every key at generation 5 for the other cases.
Smoke final means the first one or 32 write-order keys at generation 2,
with all others at generation 1.

## Schema-2 completion contract

The complete flat object contains exactly the following fields. No missing
or extra fields, duplicate JSON keys, booleans masquerading as integers,
partial output, or non-finite numeric values are accepted.

| Fields | Required value |
|---|---|
| `schema_version`, `case`, `smoke` | `2`, exact selected case, Boolean matching runner mode |
| `preparations`, `callback_invocations`, `cursor_resets` | Each `1` |
| `verifications`, `reopens` | `3`, `2` (initial, live final, reopened final verification) |
| `warmup_writes` | `N` |
| `measured_iterations` | Fixed normal count or `1` in smoke |
| `batch_size` | `32` only for `writebatch`, otherwise `1` |
| `measured_reads` | Iterations for `mixed50`, otherwise `0` |
| `measured_writes` | Iterations times batch size |
| `write_calls` | Iterations |
| `sync_write_calls` | Iterations only for `writesync`, otherwise `0` |
| `logical_write_bytes` | Measured writes times `267` |
| `record_crc32c`, `insertion_crc32c`, `present_crc32c`, `missing_crc32c` | Existing corpus goldens |
| `write_order_crc32c`, `version_values_crc32c`, `final_crc32c` | Goldens above, final computed from actual reopened records |

Raw individual Google Benchmark rows additionally contain the following
counters, with integer-valued numbers: `items_per_iteration`,
`reads_per_iteration`, `writes_per_iteration`, `batch_size`, and
`sync_writes_per_iteration`. The latter counts synchronous API calls, not
physical device synchronization operations. Normalize item time exactly as
ADR-0045 states, and cross-check these counters against the completion object.

Expected benchmark names are:

```text
ENGINE/WORKLOAD/RECORDS/iterations:I/repeats:1/process_time/real_time
```

Both `name` and `run_name` must match, and the only individual row has
`repetitions=1`, `repetition_index=0`, `threads=1`, `iterations=I`, and
`time_unit=ns`. Aggregate rows do not substitute for that individual row.
Use the existing positive-wall, finite/nonnegative-CPU, and work-rate checks.

Mutable context adds `workload_family=mutable`, `measurement_budget=fixed`,
`mutation_smoke=true|false`, `measured_sync=true|false`, `batch_size`,
`background_completion=not_drained`, and `steady_state_claimed=false`.
The runner rejects a contradiction between context, selected case, raw
counters, mode, and completion. Existing read reports remain schema 1 and
do not require these new fields.

## Background evidence

Match demangled, non-foreground stack frames by these stable function names:

| Engine | Nontrivial compaction | Flush |
|---|---|---|
| Modern | `modern_leveldb::RunCompaction(` | `modern_leveldb::FlushMemTable(` |
| LevelDB | `leveldb::DBImpl::DoCompactionWork(` | `leveldb::DBImpl::CompactMemTable(` |

Each distinct sample counts at most once per category, even when a frame
appears multiple times. A sample can include both categories when a compaction
services an intervening flush; category totals are therefore not additive.
Attribute by timestamp to four equal-duration portions of `[start, end)`,
not by sample index. Reuse existing PID/window filtering and deduplication.

The additional `background_activity` object contains:

- `measurement: "sampled_stacks_not_completed_jobs"`
- `quarters`: four objects, each with `compaction_samples` and `flush_samples`
- `sustained_compaction_observed`: all four compaction counts are positive
- `steady_state_proven: false`

No filename polling, internal engine calls, or production logging is added.
Missing symbol samples yield "not observed"; generic background-thread
activity alone is not evidence of compaction.

## Validation map

1. Add synthetic expectations for all fixed-work names, counts, fingerprints,
   ratios, modes, and schema mismatches before implementing the new family.
2. Add a benchmark-only recording diagnostic that exercises the same dispatch
   function as the real measured loops for all four normal and smoke cases.
   It checks every dispatched key, value generation, read-order position,
   read-before-write pair, batch begin/add/commit boundary, and sync argument.
   Its oracle advances from received calls, not the driver's reported counters.
   Negative checks deliberately send an incorrect permutation/generation,
   repeated read, reversed mixed pair, split batch, or missing sync request
   and require rejection. The diagnostic runs without opening a database and
   is registered as a performance CTest; no per-operation recording or hashing
   is added to the measured real adapters. Final live/reopen verification
   establishes latest-state correctness, not the absence of an intermediate
   lost write later hidden by a subsequent pass.
3. Verify invalid cases, help/listing, incompatible runner options, duplicate
   selections, changed fixed counts, and extra callbacks cannot create a
   successful result.
4. Run all 24 cases in smoke mode. Add a separate full eight-case mutable
   matrix so CI also checks four complete passes, batch equivalence, mixed
   latest-value reads, and 1,024 real sync acknowledgements.
5. Retain the existing six profiling checks; use a 600-second CTest limit
   for each smoke/full matrix and the existing owned-process limits per case.
6. Exercise synthetic background samples spanning quarters, duplicates, wrong
   threads/PIDs, boundary timestamps, missing symbols, and overlapping categories.
7. Build and run on native AppleClang and Linux GCC 13. Check the independent
   golden stream on local GCC before implementation delivery.
8. Capture normal overwrite cases for both engines after unprofiled runs;
   require verified exit, exact count, zero calibration intervals, resolved
   foreground frames, and observed compaction in all four quarters.

Record actual outcomes rather than deriving speed claims from budget probes.
If a platform cannot finish the fixed case within the stated limit, fail and
investigate; do not silently shrink work or turn a timeout into a success.

## Implementation acceptance

Implementation `b428aba` was built from a clean checkout after design-only
PR #44 merged. Its frozen native AppleClang executable has SHA-256
`d19aa6ab333858539920adacb05ba60ef1b1400220d87638222134c0acabd869`.
All eight normal mutable cases produced their exact operation budgets and
expected final-state fingerprints. The 24-case smoke matrix, recording
diagnostic, report/trace contracts, and direct ASan/UBSan executable checks
passed. Linux GCC 13 and macOS performance CI also exercised the full matrices.

Eight clean unprofiled baseline processes were followed by normal overwrite
CPU captures from that same frozen executable:

| Engine | Measured samples | Compaction samples by quarter | Flush samples by quarter |
|---|---:|---|---|
| Modern | 9,647 | 1,888 / 1,995 / 2,025 / 2,001 | 109 / 54 / 34 / 42 |
| LevelDB | 10,710 | 2,348 / 2,344 / 2,431 / 2,423 | 35 / 39 / 40 / 49 |

Both captures had exactly one complete measured interval, zero calibration
intervals, a resolved foreground loop, successful target exit, and observed
nontrivial compaction in every quarter. They confirm that the pressure
scenario exercises background work; they do not prove steady state or imply
a performance advantage between engines.

The bounded Sol implementation review identified two validation omissions:
contradictory engine/workload/record metadata and preceding calibration
intervals in mutable CPU captures. Focused regressions reproduced both before
the fixes; narrow follow-up review confirmed the corrections while preserving
old read-report/capture behavior.

Raw baselines, manifests, copied build metadata, the frozen executable, and
native captures remain local under `build/write-profiling-evidence/`. The
per-process scratch databases were removed only after verified termination.
