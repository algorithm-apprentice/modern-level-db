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
- WAL rotation, flush, and compaction pressure through a 64 KiB write buffer;
- None, Snappy, and Zstd table compression;
- versioned run, operation, final-state, completion, and failure artifacts.

V1 remains the deterministic single-thread model. V2 adds bounded concurrent
contracts; it does not claim unbounded linearizability for every API.
Process-crash safety, power-loss safety, randomized option compatibility, and
fault-injection recovery remain separate later phases in the
[stress design](local-database-stress-design.md) and
[concurrent design](concurrent-database-stress-design.md).

## Safety

Both supplied paths must:

- be absent;
- have an existing parent directory;
- be distinct and non-nested;
- refer only to disposable local stress data.

The driver rejects an existing database or artifact path. It never recursively
deletes either path and never reuses an old database.

External modification while the database is open remains unsupported.

## Build

Linux/macOS:

```bash
cmake --preset local-stress
cmake --build --preset local-stress \
  --target modern_leveldb_stress modern_leveldb_concurrent_stress
```

Windows, from an initialized x64 MSVC developer environment:

```powershell
cmake --preset windows-local-stress
cmake --build --preset windows-local-stress `
  --target modern_leveldb_stress modern_leveldb_concurrent_stress
```

Building the target refreshes the source revision and dirty-worktree identity
compiled into the executable. Rebuild after changing or committing source
before collecting evidence.

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

These campaigns are local evidence. Do not add them to GitHub Actions or CTest
without a separate cost, duration, flakiness, and ownership decision.
