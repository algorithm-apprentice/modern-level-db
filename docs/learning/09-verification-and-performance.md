# 09. Verification and Performance

[Learning path](README.md) | Next: [Guided labs](10-guided-labs.md)

Prerequisite: a first pass through [lessons 01-08](README.md).

## Different evidence answers different questions

An engine can return the right value in a simple test and still lose it
after a crash. It can decode its own output and still use the wrong format.
It can pass a benchmark while returning stale data.

The project combines independent evidence:

| Technique | Question it answers | Important limit |
|---|---|---|
| Unit tests | Does one contract handle success, boundaries, and failure? | Limited to specified cases |
| Golden bytes | Does an encoding match an independent known format? | Covers selected examples |
| Ordered-map model | Does a sequence of operations have correct logical results? | Does not model the real device |
| Differential testing | Do Modern, pinned LevelDB, and the model agree? | Agreement alone cannot rule out shared bugs |
| Deterministic power loss | Is durable ordering correct at modeled I/O boundaries? | The simulator is not every disk/kernel |
| Abrupt process exit | Does recovery work without orderly destruction? | Process failure is not machine power loss |
| Fuzzing | Can generated malformed inputs or traces violate invariants? | Finite budgets are not exhaustive |
| Sanitizers | Do exercised paths contain memory/UB/race defects? | Unexecuted paths remain unknown |
| Benchmark/profiling | What costs occur under a defined workload? | Not a universal performance ranking |

## Golden tests versus round trips

An encoder and decoder may agree on the same wrong byte order.
A round trip would pass.

Fixed LevelDB-compatible vectors prevent that shared mistake.
Tests also cover malformed lengths, unsupported tags, counts, restart
topology, checksums, and compression.
Golden database fixtures are copied into a fresh test directory before
opening; the original fixture is not modified.

## Model and differential traces

An independent ordered map tracks current visible values; snapshot model
states track older logical views.
Seeded traces combine writes, deletions, batches, reads, snapshots,
iteration, background transitions, and reopenings.

The upstream differential harness compares both implementations to the
model, not merely to each other.
It also closes databases and opens each implementation's files with the
other implementation.

Logical state and interoperable formats must agree.
Exact SSTable byte layout need not agree: compression versions and
compaction scheduling can produce different physical layouts.

Seeds make failures reproducible. A seed identifies an input trace, not a
proof that thread scheduling is always deterministic.
Small injected executors provide controlled scheduling where a test needs it.

## What the crash model promises

The crash filesystem separates:

```text
live bytes      versus synced durable bytes
live names      versus synced durable directory names
```

Append, Flush, and Close alone do not publish durable bytes in this model.
File Sync and directory Sync publish different state.
A simulated crash freezes mutation so cleanup cannot accidentally improve
the exported crash image.

Each failure boundary produces a separate image for a fresh recovery.
The oracle requires every acknowledged sync batch to survive.
An in-flight batch may be wholly present or absent, never partly applied.
Asynchronous data can be absent.

The harness also uses retained batch markers so later overwrites cannot
erase all evidence of an earlier acknowledged batch.
This model does not simulate arbitrary sector tearing, mapped-file faults, or all
storage reorderings.

## Fuzzing and sanitizers

The format fuzzer targets persistent parsers.
The stateful fuzzer runs bounded engine operations against a model.
Both need explicit input, time, allocation, and RSS limits.
Failure inputs are retained for deterministic replay.

ASan detects exercised memory misuse. UBSan checks exercised undefined
behavior. TSan detects exercised data races.
ASan/UBSan and TSan use separate builds.
The fuzz preset requires a Clang installation with libFuzzer, not merely
any executable named `clang`.

A clean run means no defect was found in that run; it does not make the
pre-alpha engine suitable for important production data.

## Practical test tiers

Run commands from the repository root. On Linux/macOS:

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug -L unit
```

From an initialized x64 MSVC developer environment on Windows:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug -L unit
```

For a learning question, prefer a narrow selector after that build:

```bash
ctest --preset dev-debug -L unit -R 'InternalKeyTest|MemTableTest'
```

```powershell
ctest --preset windows-debug -L unit -R 'InternalKeyTest|MemTableTest'
```

The remaining presets are separate, optional study sessions:

| Linux/macOS preset | Windows preset | What to study |
|---|---|---|
| `release` | `windows-release` | Behavior with optimization and Debug assertions removed |
| `compatibility` | `windows-compatibility` | Model, upstream compatibility, cross-open, and crash/process tiers |
| `asan` | Not admitted | Address/undefined-behavior instrumentation with correctness tiers |
| `tsan` | Not admitted | Race instrumentation with unit/model tiers |
| `fuzz` | Not admitted | Bounded corpus replay and fuzz smoke campaigns |
| `benchmarks` | `windows-benchmarks` | Comparative severe-regression guard and native diagnostic baseline |
| `profiling` | `windows-profiling` | Selected workloads, diagnostics, and profiling contracts |
| `coverage` | Not admitted | GCC instrumentation and changed-code coverage evidence |

See [building and testing](../development/building-and-testing.md) and
[benchmarking and profiling](../development/benchmarking-and-profiling.md)
for prerequisites, complete commands, and platform boundaries.
Do not start with every slow tier before understanding a small contract.

Changed-code coverage is a gate, not a correctness oracle.
The current CI policy requires covered changed production lines/branches,
except explicit justified exclusions. Overall coverage is reported, not
used to block unrelated legacy gaps.
Artificially executing a branch without asserting its behavior is not a
replacement for a meaningful test.

## Timing needs a workload contract

A useful comparison states:

```text
same keys and values
same logical operations and output verification
same relevant options and durability
known corpus and warmup/preconditioning
known timed boundary and unit
retained compiler, source, binary, and raw results
```

Comparing a checksummed copied read with an unchecked mapped read measures
several differences at once.
Comparing a stronger WAL-creation barrier with a weaker one can also mix
durability and speed. Controls expose such differences rather than
silently weakening the production default.

### Read-only versus mutable measurement

Read-only cases can use adaptive calibration.
Mutable workloads cannot keep retrying calibration against an increasingly
modified database.
The write harness uses fixed operation counts, one measured callback, and
a fresh process/database lifecycle per independent sample.

Batch timing includes batch construction work in the measured loop.
`mixed50` is one foreground thread performing a read and then a write;
it is not concurrent reader/writer contention or a YCSB distribution.

Acknowledged operations are measured without forcing both engines to drain
all background debt. That boundary does not establish steady state or
equal completed compaction work.

### Read the units

| Measurement | What not to infer |
|---|---|
| Average wall ns/op | Per-request P99 latency |
| Whole-process CPU | Foreground-only CPU cost |
| Logical bytes submitted | Physical bytes or write amplification |
| Smoke success | A sustained-speed result |
| Inclusive profile samples | Additive stage times or exact call counts |
| Same-machine ratio | A guarantee on another machine or workload |

Separate throughput and instrumented diagnostics binaries avoid treating
profiling overhead as production throughput.
The ordinary severe-regression benchmark's 20x bound is deliberately loose,
not a performance SLA.

## Source tour

| File or directory | Focus |
|---|---|
| [`tests/unit/`](../../tests/unit) | Small behavior and failure contracts |
| [`database_model_test.cc`](../../tests/model/database_model_test.cc) | Independent logical oracle |
| [`database_differential_test.cc`](../../tests/compatibility/database_differential_test.cc) | Cross-engine and cross-open verification |
| [`power_loss_test.cc`](../../tests/crash/power_loss_test.cc) | Durable-state enumeration |
| [`fuzz/`](../../fuzz) | Format and stateful targets |
| [`run_performance.py`](../../tools/run_performance.py) | Lifecycle/report checks and retained measurements |
| [`run_write_parity.py`](../../tools/run_write_parity.py) | Frozen multi-role write evaluation |

Read [ADR-0040](../adr/0040-compatibility-and-crash-harness.md),
[ADR-0041](../adr/0041-engine-hardening-gates.md),
[ADR-0045](../adr/0045-fixed-work-write-profiling.md), and
the current
[benchmarking and profiling guide](../development/benchmarking-and-profiling.md).
Rejected experiments are also useful: they show why a plausible local
optimization is not automatically an accepted end-to-end improvement.

## Self-check

1. Why does cross-opening database files add evidence beyond comparing Gets?
2. Why must a crash simulator freeze later cleanup?
3. Why cannot mutable calibration simply reuse the read benchmark's loop?
4. Can the production `mixed50` regression in
   [ADR-0060](../adr/0060-leveldb-write-path-parity.md) be called a speedup?

<details>
<summary>Answers</summary>

1. It exercises persistent-format interoperability and recovery, not just
   API results in each implementation's own process.
2. Otherwise unwinding might sync or delete files after the modeled crash,
   changing the image being tested.
3. Each calibration changes history, layout, and background debt, so the
   compared trials may no longer perform comparable work.
4. No. The ADR preserves that observed regression separately from the
   matched-reference improvements and explains the measurement limits.

</details>
