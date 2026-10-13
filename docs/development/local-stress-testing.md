# Local Database Stress Testing

[Development guide](README.md)

Use the local stress targets to compare native Modern LevelDB behavior with
explicit single-thread and concurrent correctness oracles.

The driver is optional and default-off. It is not a CTest, GitHub Actions job,
or scheduled repository campaign.

## Current scope

The local stress targets currently cover:

- single-thread V1:
  - Put, Delete, atomic batches, point reads, seek/full scans;
  - snapshots and release;
  - repeated close/reopen recovery;
- concurrent V2:
  - same-key Put/Delete/Get/batch histories checked for bounded linearizability;
  - concurrent disjoint writer batches;
  - snapshot point-read and iterator consistency;
  - iterators retained after Snapshot and Database handles are destroyed;
- process-crash V3:
  - parent-owned sync acknowledgement journal;
  - abrupt kill/reap after every acknowledged batch;
  - reopen and exact prefix verification after every child;
  - optional whole-batch in-flight probes;
- option/fault V4:
  - complete 48-cell public option matrix;
  - eleven named in-memory I/O/scheduling fault boundaries;
  - retry, sticky-stop, and whole-batch recovery oracles;
  - separate fault-free liveness with deterministic background draining;
- campaign orchestration V5:
  - deterministic multi-seed V1 through V4 job plans;
  - bounded owned subprocesses and retained attempt bundles;
  - source-identity validation, durable summaries, and exact-plan resume;
- WAL rotation, flush, and compaction pressure through a 64 KiB write buffer;
- None, Snappy, and Zstd table compression;
- versioned run, operation, final-state, completion, and failure artifacts.

V1 remains the deterministic single-thread model. V2 adds bounded concurrent
contracts; it does not claim unbounded linearizability for every API. V3 adds
process-crash acknowledgement/recovery evidence, not machine power-loss
certification. V4 composes a bounded supported option matrix with deterministic
fault seams; it does not model arbitrary partial syscalls or native media
failure. V5 composes the existing tools without adding another database oracle.

## Safety

V1 through V3 database/output paths must:

- be absent;
- have an existing parent directory;
- be distinct and non-nested;
- refer only to disposable local stress data.

The driver rejects an existing database or artifact path. It never recursively
deletes either path and never reuses an old database.

V4 takes one `--workspace` path. It must be absent and have an existing parent.
The executable creates `databases/` and `artifacts/` beneath it and never
deletes or reuses the workspace.

External modification while the database is open remains unsupported.

## Build

Linux/macOS:

```bash
cmake --preset local-stress
cmake --build --preset local-stress \
  --target modern_leveldb_stress modern_leveldb_concurrent_stress \
    modern_leveldb_crash_stress_worker modern_leveldb_option_fault_stress
```

Windows, from an initialized x64 MSVC developer environment:

```powershell
cmake --preset windows-local-stress
cmake --build --preset windows-local-stress `
  --target modern_leveldb_stress modern_leveldb_concurrent_stress `
    modern_leveldb_crash_stress_worker modern_leveldb_option_fault_stress
```

Building the target refreshes the source revision and dirty-worktree identity
compiled into the executable. Rebuild after changing or committing source
before collecting evidence.

V5 is a Python 3.9+ runner and adds no build target. Build all four stress
executables above before starting a campaign.

## Run

Start with a short disposable run. Linux/macOS:

```bash
./build/local-stress/stress/modern_leveldb_stress \
  --db /absolute/disposable/path/stress-db-01 \
  --output /absolute/disposable/path/stress-output-01 \
  --seed 301 \
  --operations 1000 \
  --compression none
```

Windows:

```powershell
& .\build\windows-local-stress\stress\modern_leveldb_stress.exe `
  --db C:\absolute\disposable\path\stress-db-01 `
  --output C:\absolute\disposable\path\stress-output-01 `
  --seed 301 `
  --operations 1000 `
  --compression none
```

The default operation count is 100,000. The operation log hex-encodes complete
generated keys and values and can be large; choose the count and available
storage deliberately.

Supported options:

| Option | Default | Meaning |
|---|---:|---|
| `--db` | required | New disposable database directory |
| `--output` | required | New artifact directory |
| `--seed` | 301 | Reproducible `mt19937_64` workload seed |
| `--operations` | 100000 | Positive operation count |
| `--compression` | `none` | `none`, `snappy`, or `zstd` |

Use `--help` to print the command contract.

## Run concurrent V2

Linux/macOS:

```bash
./build/local-stress/stress/modern_leveldb_concurrent_stress \
  --db /absolute/disposable/path/concurrent-db-01 \
  --output /absolute/disposable/path/concurrent-output-01 \
  --seed 301 \
  --threads 4 \
  --rounds 1000 \
  --compression none
```

Windows:

```powershell
& .\build\windows-local-stress\stress\modern_leveldb_concurrent_stress.exe `
  --db C:\absolute\disposable\path\concurrent-db-01 `
  --output C:\absolute\disposable\path\concurrent-output-01 `
  --seed 301 `
  --threads 4 `
  --rounds 1000 `
  --compression none
```

Concurrent options:

| Option | Default | Meaning |
|---|---:|---|
| `--db` | required | New disposable database directory |
| `--output` | required | New artifact directory |
| `--seed` | 301 | Reproducible operation/yield selection |
| `--threads` | 4 | Worker count in the bounded range 2 through 8 |
| `--rounds` | 1000 | Linearizability rounds and batch generations |
| `--compression` | `none` | `none`, `snappy`, or `zstd` |

Each linearizability round runs one operation per thread against one hot key,
then performs a final read and searches for a legal sequential witness that
respects real-time precedence. The second phase uses disjoint writer pairs to
check atomic batch, snapshot, iterator, and retained-child lifetimes.

The same seed chooses the same operations and yield counts. Native scheduling
can still produce a different valid history; `linearizability.log` records the
actual invocation/completion intervals and witness.

## V1 artifacts

| File | Interpretation |
|---|---|
| `run.txt` | Stress format, source revision/dirty state, seed, operation count, compression, and fixed policy |
| `operations.log` | Attempted operations in order; each record is flushed before execution |
| `final-state.txt` | Sorted hex key/value state after final reopen and forward/reverse verification |
| `completion.txt` | Exists only after the complete run succeeds |
| `failure.txt` | Best available failure message when the process remains able to report it |

`completion.txt` is the success authority. A partially written final-state file
does not make a failed run successful.

To reproduce a normal V1 failure, rebuild the same source revision and run the
same seed, operation count, compression, and fixed policy with fresh database
and output paths. The operation log identifies the final attempted step even if
a later workload format changes.

The tool returns:

| Code | Meaning |
|---:|---|
| 0 | Completed and verified |
| 1 | Database, model, filesystem, or artifact failure |
| 2 | Invalid command line or unsafe paths |

## V2 artifacts

| File | Interpretation |
|---|---|
| `run.txt` | Concurrent format, source identity, seed, threads, rounds, compression, and fixed policy |
| `linearizability.log` | Per-round initial state, intervals/results, final read, witness order, and final value |
| `snapshot-summary.txt` | Writer count, acknowledged batch writes, and snapshot/iterator checks |
| `final-state.txt` | Reopened final state after all child handles release |
| `completion.txt` | Exists only after both concurrent phases succeed |
| `failure.txt` | Best available failure message |

The concurrent executable uses the same exit-code meanings as V1.

## Run process-crash V3

Linux/macOS:

```bash
python3 tools/run_crash_stress.py \
  --worker ./build/local-stress/stress/modern_leveldb_crash_stress_worker \
  --db /absolute/disposable/path/crash-db-01 \
  --output /absolute/disposable/path/crash-output-01 \
  --rounds 25 \
  --seed 301 \
  --compression none \
  --timeout-seconds 30 \
  --inflight-probes 3
```

Windows:

```powershell
python tools\run_crash_stress.py `
  --worker .\build\windows-local-stress\stress\modern_leveldb_crash_stress_worker.exe `
  --db C:\absolute\disposable\path\crash-db-01 `
  --output C:\absolute\disposable\path\crash-output-01 `
  --rounds 25 `
  --seed 301 `
  --compression none `
  --timeout-seconds 30 `
  --inflight-probes 3
```

For every acknowledged round, the worker synchronizes one atomic batch,
emits an acknowledgement, and remains alive with its Database handle. The
parent fsyncs that acknowledgement into an external journal, kills and reaps
the child, then starts a fresh verifier. The verifier must recover the complete
journal prefix.

In-flight probes emit readiness before attempting a batch. Recovery may report
the batch wholly absent or wholly complete; a partial batch is failure.

V3 requires Python 3.9 or newer. The parent owns all child processes and
applies bounded protocol and completion timeouts.

## V3 artifacts

| File | Interpretation |
|---|---|
| `run.txt` | Worker source identity, seed, rounds, compression, timeout, and probe count |
| `acknowledged.log` | Parent-owned contiguous oracle, flushed and fsynced before each kill |
| `process.log` | Launch, journal, kill, reap/verification progression |
| `inflight.txt` | Each optional probe's absent/complete result |
| `completion.txt` | Exists only after every recovery and optional probe succeeds |
| `failure.txt` | Parent failure message |

The probe databases remain under the output directory. Preserve a failure
directory unchanged until the worker command and journal reproduce it.

## Run option and fault matrix V4

Linux/macOS:

```bash
./build/local-stress/stress/modern_leveldb_option_fault_stress \
  --workspace /absolute/disposable/path/option-fault-01 \
  --seed 301 \
  --operations 250 \
  --liveness-writes 2000
```

Windows:

```powershell
& .\build\windows-local-stress\stress\modern_leveldb_option_fault_stress.exe `
  --workspace C:\absolute\disposable\path\option-fault-01 `
  --seed 301 `
  --operations 250 `
  --liveness-writes 2000
```

V4 always runs all 48 option cells and all eleven fault scenarios. `--operations`
is the shared-model operation count per native cell. `--liveness-writes` is the
separate no-fault internal-engine write count.

V4 options:

| Option | Default | Meaning |
|---|---:|---|
| `--workspace` | required | New disposable campaign root |
| `--seed` | 301 | Campaign seed and per-cell seed source |
| `--operations` | 250 | Positive model operations per option cell |
| `--liveness-writes` | 2000 | Positive writes in the fault-free liveness phase |

The option dimensions are compression (None/Snappy/Zstd), mapped/copy reads,
Bloom disabled/10 bits per key, 64/256 KiB write buffers, and synchronized or
unsynchronized WAL creation.

The fault phase uses fresh in-memory databases for read, WAL append/sync/close,
table sync/close/directory-sync, MANIFEST append/sync, and scheduling
rejection. A failed mutation must become sticky, a later write must return the
same error without filesystem progress, and reopen must show one complete
allowed batch state.

## V4 artifacts

| File | Interpretation |
|---|---|
| `run.txt` | Source identity, seed, budgets, and selected matrix/scenario counts |
| `cells.tsv` | Stable cell/options ID, derived seed, entry count, and state fingerprint |
| `faults.tsv` | Injected operation, error, sticky/retry result, and reopened batch state |
| `liveness.txt` | Writes, drained tasks, table outputs, reads/scans, and final fingerprint |
| `completion.txt` | Exists only after every cell, fault, and liveness oracle succeeds |
| `failure.txt` | Best available failure message |

Each native database remains under `workspace/databases/<cell-id>`. Preserve
the complete workspace when any phase fails.

## Run multi-tier campaign orchestration V5

Linux/macOS:

```bash
python3 tools/run_local_stress_campaign.py \
  --stress-dir ./build/local-stress/stress \
  --workspace /absolute/disposable/path/campaign-01 \
  --seeds 1,17,301 \
  --tiers v1,v2,v3,v4 \
  --v1-operations 5000 \
  --v2-rounds 300 \
  --v3-rounds 25 \
  --v3-inflight-probes 3 \
  --v4-operations 250 \
  --v4-liveness-writes 2000
```

Windows:

```powershell
python tools\run_local_stress_campaign.py `
  --stress-dir .\build\windows-local-stress\stress `
  --workspace C:\absolute\disposable\path\campaign-01 `
  --seeds 1,17,301 `
  --tiers v1,v2,v3,v4 `
  --v1-operations 5000 `
  --v2-rounds 300 `
  --v3-rounds 25 `
  --v3-inflight-probes 3 `
  --v4-operations 250 `
  --v4-liveness-writes 2000
```

For every seed, the full plan contains three V1 compression jobs, the V2
2/None, 4/Snappy, and 8/Zstd profiles, three V3 compression jobs, and one V4
job. The default three seeds therefore produce 30 sequential subprocesses.

Campaign options:

| Option | Default | Meaning |
|---|---:|---|
| `--stress-dir` | required | Directory containing all four current-platform stress executables |
| `--workspace` | required | New campaign root, or recognized root with `--resume` |
| `--seeds` | `1,17,301` | Ordered, unique uint64 seeds |
| `--tiers` | `v1,v2,v3,v4` | Complete tiers to include |
| `--v1-operations` | 5000 | V1 operations per compression/seed |
| `--v2-rounds` | 300 | V2 rounds per thread/compression profile/seed |
| `--v3-rounds` | 25 | V3 acknowledged crash rounds per compression/seed |
| `--v3-inflight-probes` | 3 | V3 optional in-flight probes per job |
| `--v3-timeout-seconds` | 30 | V3 child protocol timeout |
| `--v4-operations` | 250 | V4 operations per native option cell |
| `--v4-liveness-writes` | 2000 | V4 fault-free liveness writes |
| `--job-timeout-seconds` | 3600 | Outer timeout for one complete job |
| `--max-duration-seconds` | 0 | Invocation budget; zero is unlimited |
| `--resume` | off | Revalidate and continue an exact recorded plan |

Resume with the same planning arguments and stress directory:

```console
python3 tools/run_local_stress_campaign.py ... --resume
```

Passed jobs are skipped only after their completion format and normalized
revision/dirty identity are revalidated. Failed, timed-out, interrupted, or
stale-running jobs receive a new attempt directory; earlier attempts are never
deleted or overwritten.

## V5 artifacts

| Path | Interpretation |
|---|---|
| `plan.json` | Canonical immutable job plan and SHA-256 digest |
| `events.jsonl` | Append-only fsynced campaign transition journal |
| `state.json` | Current durable job/attempt state |
| `summary.json` | Overall, per-tier, per-seed, duration, identity, and attempt summary |
| `summary.txt` | Human-readable summary |
| `completion.txt` | Exists only after every planned job passes |
| `jobs/<index>-<id>/state.json` | Durable state for one job |
| `jobs/<index>-<id>/attempts/<n>/` | Exact command, logs, result, validation, database, and stress artifacts |

The orchestrator stops at the first blocking job. Exit code 3 means the
invocation-wide duration budget expired with resumable pending work; it is not
a completed campaign. Exit code 130 means user interruption after owned-process
containment.

## Local acceptance sets

Before reviewing V1 changes, run three fresh campaigns:

```text
compression  seed
none         1
snappy       17
zstd         301
```

Use an operation count large enough for the machine and review goal. Preserve
any failure directory unchanged until the seed and operation index reproduce.

Before reviewing V2 changes, run fresh combinations covering:

```text
threads  compression  seed
2        none         1
4        snappy       17
8        zstd         301
```

Repeat at least one combination several times because scheduling, unlike the
operation plan, is intentionally nondeterministic. Preserve every invalid
history unchanged.

Before reviewing V3 changes, run at least 25 acknowledged kill/recover rounds
plus in-flight probes for None, Snappy, and Zstd. On Windows, include one
Unicode database/output path. Confirm no worker process survives any campaign.

Before reviewing V4 changes, run all 48 cells, all eleven fault scenarios, and at
least 2,000 liveness writes for seeds 1, 17, and 301. Use a fresh workspace for
each campaign and preserve every failure workspace unchanged.

Before reviewing V5 changes, run a low-budget all-tier campaign, preserve and
resume one real failed attempt, exercise a contained timeout, and run at least
one equivalent campaign on POSIX. Confirm every completed job reports one
source identity and no process survives timeout or interruption.

These campaigns are local evidence. Do not add them to GitHub Actions or CTest
without a separate cost, duration, flakiness, and ownership decision.
