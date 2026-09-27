# ADR-0045: Fixed-Work Write and Mixed Profiling

## Status

Accepted design. Merge this design before the separate implementation PR.

## Context and research

ADR-0044 removed the measured CRC bottleneck without changing persistent
formats. The next task is to measure write-path and background-compaction
costs, not to copy RocksDB's concurrency or compaction strategies speculatively.

ADR-0042's sixteen read cases remain unchanged. Their adaptive calibration
and same-process repetitions are unsuitable for mutable workloads: every
calibration attempt would advance database contents and compaction history,
and a faster implementation could perform more preparatory writes.

Primary sources and isolated probes established the following:

| Question | Evidence | Decision |
|---|---|---|
| Mature workload concepts | LevelDB `db_bench` provides overwrite, batch, sync, and read-while-writing cases; RocksDB also provides mixed read/write cases | Reuse concepts, not either complete driver |
| Is a 50/50 mix YCSB A? | YCSB A uses Zipfian requests and multi-field records | Call our uniform, whole-value, single-foreground-thread case `mixed50`, not YCSB |
| Can Google Benchmark avoid calibration? | At pinned v1.9.5, `Iterations(N)->Repetitions(1)` executes one fixed-count callback and overrides CLI time/count/repetition settings | Use fixed work for this new family and fresh processes for independent samples |
| What does the JSON name become? | An 8,192-batch probe reports `/iterations:8192/repeats:1/process_time/real_time` | Validate the actual fixed-count name, not the read-family suffix |
| Can flags silently change work? | A dry-run override or framework warmup was rejected by callback-count/iteration checks | Require exactly one callback at the expected count; fail instead of accepting a mutated calibration state |
| Is the work budget practical? | Eight native API/framework probes completed; non-sync measured loops took about 18-34 seconds, sync loops about four seconds, preparation about 0.4-14 seconds | Freeze the tested budgets below; no time-based stop or automatic retuning |
| Does the pressure case actually compact? | Two native Time Profiler captures observed flush and nontrivial compaction frames in every quarter of the measured overwrite window, for both engines | Retain this bounded pressure workload and expose sampled background evidence |
| Does close drain compaction debt? | Neither public API exposes a common drain barrier; Modern's close can stop ongoing compaction | Measure acknowledged operations, not completed background work; make the limitation explicit |
| Are sync options comparable? | Both POSIX implementations use full-fsync on supporting macOS filesystems, with fallback, and fdatasync/fsync on Linux | Keep native `sync=true`; do not simulate synchronization or claim a latency SLA |

The probes link the existing pinned libraries without changing production
or benchmark source. Raw outputs are local under
`build/write-profiling-research/`.

## Decision

### Scope and fixed cases

Extend the existing executable, runner, and CPU collector. Add eight cases:
each row below runs against both `modern` and the pinned `leveldb`.

| Workload | Records | One benchmark iteration | Normal iterations | Measured writes | Measured reads |
|---|---:|---|---:|---:|---:|
| `overwrite` | 65,536 | One async `Put` | 262,144 | 262,144 | 0 |
| `writebatch` | 65,536 | Clear/build/commit one 32-key async batch | 8,192 | 262,144 | 0 |
| `writesync` | 4,096 | One `Put(sync=true)` | 1,024 | 1,024 | 0 |
| `mixed50` | 65,536 | One successful Get, then one async Put | 262,144 | 262,144 | 262,144 |

There is one foreground thread. Background engine threads run normally.
The mix is serialized application traffic, not concurrent read/write
contention, group commit, a rate-limited load generator, or tail-latency data.
Those are separate future workload families.

All settings from ADR-0042 remain: 256-byte values, 8 MiB cache, 64 KiB write
buffer, 4 KiB blocks, restart interval 16, Snappy, no Bloom filter, WAL enabled,
and verified read checksums. Do not change LevelDB's disabled external CRC
configuration or either engine's background policies.

The existing cases, corpus fingerprints, timed operations, calibration,
repetitions, and schema-1 completion reports do not change.

### Deterministic updates and bounded state

Reuse the original keys, base values, insertion order, and read order.
Add a write permutation using ADR-0042's explicit Fisher-Yates algorithm
with `mt19937_64(305)`. Repeat that permutation on each complete write pass.

Precompute value generations 1 through 5 outside timing: each is the original
256-byte value with its first eight bytes replaced by the generation as LE64.
The original value is generation 0 and remains unchanged. This keeps data
generation out of timing and makes stale latest/final values observable.
Final verification alone cannot detect every earlier lost update that a later
pass overwrites; it is not a transaction-history proof. Do not overwrite a key
with an indistinguishable value.

Preconditioning writes generation 1 to every key once, in write order.
Measured write ordinal `w` targets `write_order[w % N]` and writes generation
`2 + floor(w / N)`. Batches contain consecutive write ordinals, so the batch
and single-write cases issue the same logical update stream. Normal async
cases finish at generation 5 for every key. Sync updates only the first 1,024
keys of its write permutation to generation 2.

Mixed iteration `j` first reads `present_order[j % N]` and checks its complete
current value, then writes ordinal `j`. A per-key generation array is updated
only after a successful Put or batch commit. The timed mix therefore includes
full read-value comparison and model bookkeeping; it is not directly
comparable to the existing presence-only read benchmark.

Live cardinality never grows. Fixed operation counts bound logical writes,
but physical disk usage still depends on normal compaction. The existing
owned-process timeout limits stuck runs; no engine-specific flush, manual
compaction, sleep-based quiescence, or host cache purge is introduced.

### Lifecycle, timing, and independent repetitions

One process owns one selected case and one fresh runner-owned database:

1. Lazily create the fixture only after the framework matches the case.
   Before opening files, require the expected fixed iteration count and the
   first callback invocation.
2. Generate corpus, versions, write order, and the generation model. Populate
   generation 0 in original insertion order with async single-key writes.
3. Close/reopen and verify all generation-0 contents with `fill_cache=false`.
4. Perform one complete async single-key preconditioning pass at generation 1.
   Do not close, sleep, or force quiescence afterward.
5. Reset cursors once. Emit the existing benchmark-local marker and run the
   fixed operation budget with wall and whole-process CPU timers.
6. After measurement, verify all current contents, close/reopen, verify again,
   and close. Persist the completion report only on success.
7. The runner verifies reports and process termination before removing its
   owned scratch directory. Preserve diagnostic artifacts on failure.

Batch object construction is outside timing; `Clear`, all 32 `Put` encodings,
`Write`, status handling, and generation-model updates are inside timing.
Single-key cases use each public `Put` naturally. Do not prebuild batches,
disable the WAL, or put verification/reopen work inside the measured loop.

Use `Iterations(fixed)->Repetitions(1)->UseRealTime()->MeasureProcessCPUTime()`.
No adaptive calibration or framework warmup may successfully execute.
Reject a second callback and unexpected iteration counts. The runner rejects
`--min-time` and repetition counts other than one for mutable cases. Collect
multiple samples by invoking it with distinct output directories and fresh
processes; alternate engines or before/after variants in a predetermined order.

Smoke mode uses exactly one iteration of the same operation and the full
preparation lifecycle. It is explicitly marked as smoke and is not a speed
measurement or proof of sustained compaction. The executable's new `--smoke`
applies only to mutable cases and cannot combine with profile markers. Existing
read smoke behavior remains driven by the framework's one-iteration setting.

### Meaning and units of results

`items_per_iteration` is 1 for single writes, 32 for batches, and 2 for the
mixed pair. Report exact measured read/write counts, batch size, sync-write
call count, and logical key-plus-value bytes submitted. One key is 11 bytes;
each write contributes 267 logical bytes. These are not physical bytes or
write-amplification statistics. A mixed `ns/item` is an average over its read
and write operations, not either operation's latency; retain the raw per-pair
time as well.

This is a **preconditioned, bounded update workload**. It is not a claim of
steady state, drained compaction debt, final SST layout equality, sustained
device bandwidth, or per-request P99. Background CPU that overlaps the
measurement is included; outstanding work after the last acknowledgement is
excluded. Reopen verification establishes logical correctness, not completion
of every possible background compaction.

Keep ordinary execution bounded by the runner's 300-second timeout. CPU
capture retains the 120-second recording limit and 180-second outer deadline.
A run exceeding its budget fails rather than reducing work. Sync captures may
contain few CPU samples while waiting on storage; do not substitute CPU sample
weight for durability latency.

### Reports and CPU attribution

Preserve raw Google Benchmark JSON and the outer manifest. Add schema-2
completion reports only for the new family, with strict lifecycle, operation,
and fingerprint validation. The runner verifies exact fixed iteration names,
one repetition, expected counters, and consistency between both reports.
The detailed schema and fingerprint goldens are in
[the implementation handoff](../write-profiling-design.md).

Add mutable-case context for fixed-work/smoke mode, measured sync policy,
batch size, no background drain, and no steady-state claim. Existing dependency,
source/dirty state, compiler, binary-hash, collector, and cleanup provenance
remain mandatory.

Extend workload-symbol recognition for the four new measured-loop functions.
For mutable captures, report sampled non-foreground flush and nontrivial
compaction frames in four equal-duration quarters of the verified interval.
Call sustained compaction **observed** only when all four quarters contain
matching compaction samples. Missing samples mean "not observed", not proof
that compaction did not occur, and do not make a valid sync capture fail.
These are sampled call stacks, not completed-job counters or steady-state
proof. The acceptance exercise must observe both engines compacting in each
quarter of the normal overwrite case.

Native traces remain local. No production instrumentation, statistics API,
allocator, new dependency, workload-generator framework, or engine optimization
belongs to this change.

## Validation and delivery

- Verify independent corpus/update/final-state goldens across standard libraries.
- Drive the same per-iteration dispatcher with a benchmark-only recording
  adapter in both normal and smoke modes. Independently check the actual
  dispatched key/generation order, read-before-write pairs, batch boundaries,
  and sync arguments; generated fingerprints and self-reported counters alone
  do not establish the executed stream.
- Exercise every new case in smoke and normal fixed-work modes, preserving all
  old read contracts and the legacy severe-regression gate.
- Reject wrong counts/ratios, missing lifecycle work, malformed reports,
  changed fingerprints, skipped operations, and fake success after errors.
- Cover fixed-count override behavior, invalid selection/help without data
  creation, incompatible runner options, and profile-marker mismatches.
- Test background-quarter attribution with synthetic traces; complete real
  overwrite captures for both engines and retain the evidence locally.
- Use the existing Linux/macOS performance CI gate, with bounded separate
  smoke and full mutable matrices. No noisy throughput threshold is added.
- Obtain one bounded Sol design review, merge this design-only PR, implement
  separately, and obtain a bounded Sol implementation review before delivery.

## References

- [Google Benchmark v1.9.5 runner](https://github.com/google/benchmark/blob/192ef10025eb2c4cdd392bc502f0c852196baa48/src/benchmark_runner.cc)
- [Google Benchmark user guide](https://google.github.io/benchmark/user_guide.html)
- [Pinned LevelDB workloads](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/benchmarks/db_bench.cc)
- [RocksDB benchmarking concepts](https://github.com/facebook/rocksdb/wiki/Benchmarking-tools)
- [YCSB workload A](https://github.com/brianfrankcooper/YCSB/blob/master/workloads/workloada)
- [ADR-0042 read measurement contract](0042-benchmark-profiling-foundation.md)
- [ADR-0044 completed checksum optimization](0044-profile-guided-crc32c-acceleration.md)
