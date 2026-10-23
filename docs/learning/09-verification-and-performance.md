# 09. Verification and Performance

[Learning path](README.md) | Next: [Guided labs](10-guided-labs.md)

Prerequisite: a first pass through [lessons 01-08](README.md).

> **Advanced engineering lesson:** use this after understanding the ordinary
> engine paths. Its goal is to select evidence, not to teach storage semantics
> for the first time.

## Lesson map

| Question | Answer |
|---|---|
| What problem is this lesson about? | Choosing evidence that can falsify correctness or performance claims at the right boundary |
| Who consumes this evidence? | Contributors, reviewers, and maintainers deciding whether a change is safe and useful |
| What interface matters first? | Test tiers, CI gates, benchmark workload contracts, reports, and retained artifacts |
| What implementation will we open? | Golden/model/differential/crash tests, fuzzing, sanitizers, calibration, and profiling tools |
| What should you be able to do afterward? | Match a claim to the evidence needed and reject measurements that compare different work |

## Different evidence answers different questions

An engine can return the right value in a simple test and still lose it
after a crash. It can decode its own output and still use the wrong format.
It can pass a benchmark while returning stale data.

The project combines independent evidence:

First express a claim as something an observation could falsify. "The engine
is safe" has no bounded oracle. "Every acknowledged sync batch survives each
crash cut in this filesystem model" identifies acknowledged markers,
failure points, and allowed recovery states.

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
| `release` | `windows-release` | Behavior under optimization; production runtime assertions are absent in both configurations |
| `compatibility` | `windows-compatibility` | Model, upstream compatibility, cross-open, and crash/process tiers |
| `asan` | Not admitted | Address/undefined-behavior instrumentation with correctness tiers |
| `tsan` | Not admitted | Race instrumentation with unit/model tiers |
| `fuzz` | Not admitted | Bounded corpus replay and fuzz smoke campaigns |
| `benchmarks` | `windows-benchmarks` | Comparative severe-regression guard and native diagnostic baseline |
| `profiling` | `windows-profiling` | Selected workloads, CPU capture, and profiling contracts |
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
| Wall time / total operations with several readers | Individual request latency |
| Same-machine ratio | A guarantee on another machine or workload |

Separate throughput and instrumented diagnostics binaries avoid treating
profiling overhead as production throughput.
The ordinary severe-regression benchmark's 20x bound is deliberately loose,
not a performance SLA.

### Correctness, generated work, and timing are separate claims

An implementation can preserve behavior while changing the work needed to
express it. Keep three evidence layers separate:

| Claim | Evidence to seek | What remains unproved |
|---|---|---|
| The bytes and outcomes are unchanged | Independent literal/Native oracles, malformed-input and lifetime tests | Runtime cost |
| A wrapper or checked-codec call disappears | Actual optimized callers and linked bodies, with matching headers and symbols | Whole-Get speed or a per-call cycle saving |
| The integrated workload improves | Verified equal work, paired runs, negative controls, and all raw observations | Each changed mechanism's causal share |

An ABI-changing candidate needs callers compiled against its matching
headers, not an old driver object forced onto new library symbols. The
compiled interface/layout change is then part of the treatment.

A flat profile can also hide equivalent work under different names. One
engine may outline a word decoder while another inlines it into Seek.
The first engine's decoder self percentage is not the fraction of the gap
caused by decoding. Compare source-matched caller phases and assign each
sample once; inclusive totals overlap. Even an exclusive phase difference
is diagnostic evidence, not the saving an intervention will necessarily
recover. Keep unclassified and unsampled CPU explicit rather than assigning
it to a guessed cause.

### Negative controls are not a correction factor

A/A controls execute the same program without the proposed mechanism
change. Differences can still expose host load, placement, cache, or
preconditioning variation. An unchanged function in a relinked executable
is not the same control: its placement may have changed.
Keep systematic drift and adverse observations visible. Do not subtract
control drift to manufacture a candidate gain or resample until it wins.
Observed A/A variation is descriptive, not a confidence bound proving
equivalence.

### Work a reporting example before choosing a summary

Use two fixed-work rounds with cost units:

<!-- paired-cost-example -->

| Round | Before cost | After cost |
|---|---|---|
| 1 | 100 | 90 |
| 2 | 200 | 210 |

Within-round ratios are 0.90 and 1.05. Their median is 0.975, a cost change
of -2.5 percent. The ratio of aggregate medians is 150/150, or no change.
These summaries answer different questions; silently substituting one for
the other changes the report.

Rates have another nonlinear transformation. Median per-round `1/cost`
is not generally `1/median(cost)`. With these before costs the former is
0.0075 and the latter about 0.00667 in reciprocal cost units. State the
estimator, preserve pair identity, and do not label reciprocal median cost
as median measured throughput.

### A combination needs an incremental comparison

Combinations need a second distinction. For identical fixed work, let
`T` be elapsed cost and define interaction as:

```text
I = T(A+B) - T(A) - T(B) + T(base)
```

Consider illustrative costs, not measurements:

| Variant | Cost units |
|---|---:|
| Base | 100 |
| A | 90 |
| B | 120 |
| A+B | 90 |

Interaction is negative, but A+B is no faster than A. The combination
merely avoids B's standalone harm. Report the direct A+B-versus-A contrast
as well as the interaction and baseline comparison.

Conversely, establishing a coherent reference architecture need not require
each small mechanism to win its own noisy benchmark. Review the required
invariants, verify the integrated generated work, and measure the declared
composition. Its gain still cannot be divided into individual contributions
or added to percentages from another campaign.

[ADR-0090](../adr/0090-fixed-word-decoding.md) and
[ADR-0091](../adr/0091-native-get-cost-model-parity.md) preserve the concrete
negative controls, rejected interpretations, and integrated results. They
are the evidence records; the transferable lesson is how to distinguish
these claims.

## Exercises: construct evidence that can reject your claim

1. Why does cross-opening database files add evidence beyond comparing Gets?
2. Why must a crash simulator freeze later cleanup?
3. Why cannot mutable calibration simply reuse the read benchmark's loop?
4. Can the production `mixed50` regression in
   [ADR-0060](../adr/0060-leveldb-write-path-parity.md) be called a speedup?
5. A helper microbenchmark improves while the end-to-end workload is
   unchanged. What claim is justified?
6. Only one engine has a decoder self symbol. Can its profile share explain
   the performance gap without examining callers?
7. In the illustrative combination above, does negative interaction prove
   that adding B to A improves throughput?
8. **Oracle:** design a test where an encoder and decoder share an endian
   bug. Which independent evidence detects it?
9. **Calculate:** reproduce both summaries of the paired-cost table and
   explain why their disagreement is not an arithmetic error.
10. **Design:** compare an ABI-changing read implementation with its
    baseline. List the source/header, caller, provider, image, and timing
    identities you must retain.
11. **Transfer:** propose a counterexample to a performance claim that
    reports only the best round. Design a predeclared protocol that would
    keep that counterexample visible.

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
5. Only that the isolated helper improved under its defined workload. The
   evidence does not establish an end-to-end application improvement.
6. No. The other engine can charge inlined work to a caller. Comparable
   phases help locate cost but do not replace a controlled intervention.
7. No. A+B and A both cost 90. The direct incremental comparison shows no
   benefit from adding B despite negative interaction.
8. A big-endian encoder and matching decoder can round-trip. Literal
   little-endian golden bytes or an independently checked native reference
   reject the shared mistake.
9. The paired estimator is -2.5 percent; the ratio of medians is zero.
   One aggregates matched ratios, the other aggregates costs before taking
   their ratio. Record which question each answers.
10. Rebuild each caller against matching headers; retain ordinary options,
    providers, binaries/symbols, dataset/policies/work, and all raw samples.
    Do not force an old object across incompatible symbols to appear fair.
11. A favorable noisy sample can win even when most pairs regress.
    Fixed work/order, retained A/A/adverse observations, and publication
    rules before measurement prevent choosing only that sample afterward.

</details>

## Source tour

| Entry point | Interface or symbol | Question to answer | Representative evidence |
|---|---|---|---|
| [`tests/unit/`](../../tests/unit) | focused contracts and fault seams | What is the smallest deterministic test for this behavior? | changed-code coverage and platform unit jobs |
| [`database_model_test.cc`](../../tests/model/database_model_test.cc) | ordered-map oracle | Which logical histories agree independently of file layout? | model CI job |
| [`database_differential_test.cc`](../../tests/compatibility/database_differential_test.cc) | cross-engine and cross-open checks | Can both implementations read each other's durable state? | compatibility CI job |
| [`power_loss_test.cc`](../../tests/crash/power_loss_test.cc) | durable-state enumeration | Which states are legal at each mutation boundary? | crash CI job |
| [`fuzz/`](../../fuzz) | generated formats and operation traces | Which invariants survive generated malformed inputs or histories? | fuzz and sanitizer jobs |
| [`run_performance.py`](../../tools/run_performance.py) | workload/report lifecycle | Are compared trials doing the same work under the same contract? | performance jobs and retained reports |
| [`run_write_parity.py`](../../tools/run_write_parity.py) | matched write evaluation | Which CPU and wall-time differences remain against pinned LevelDB? | benchmark/profiling artifacts |

Read [ADR-0040](../adr/0040-compatibility-and-crash-harness.md),
[ADR-0041](../adr/0041-engine-hardening-gates.md),
[ADR-0045](../adr/0045-fixed-work-write-profiling.md), and
the current
[benchmarking and profiling guide](../development/benchmarking-and-profiling.md).
Rejected experiments are also useful: they show why a plausible local
optimization is not automatically an accepted end-to-end improvement.
