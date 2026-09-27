# Write Profiling Research and Implementation Handoff

This document implements the decisions in
[ADR-0045](adr/0045-fixed-work-write-profiling.md). It extends the existing
profiling harness, not the engine or its public API.

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
