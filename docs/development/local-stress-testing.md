# Local Database Stress Testing

[Development guide](README.md)

Use the local stress driver to run a reproducible single-threaded operation
stream against a native Modern LevelDB database and an independent ordered-map
model.

The driver is optional and default-off. It is not a CTest, GitHub Actions job,
or scheduled repository campaign.

## Current scope

V1 covers:

- Put, Delete, atomic batches, point reads, seek/full scans;
- snapshots and release;
- repeated close/reopen recovery;
- WAL rotation, flush, and compaction pressure through a 64 KiB write buffer;
- None, Snappy, and Zstd table compression;
- versioned run, operation, final-state, completion, and failure artifacts.

V1 is single-threaded. It does not establish concurrent correctness,
process-crash safety, power-loss safety, randomized option compatibility, or
fault-injection recovery. Those are separate later phases in the
[stress design](local-database-stress-design.md).

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
cmake --build --preset local-stress --target modern_leveldb_stress
```

Windows, from an initialized x64 MSVC developer environment:

```powershell
cmake --preset windows-local-stress
cmake --build --preset windows-local-stress --target modern_leveldb_stress
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

## Artifacts

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

## Local acceptance set

Before reviewing stress-driver changes, run three fresh campaigns:

```text
compression  seed
none         1
snappy       17
zstd         301
```

Use an operation count large enough for the machine and review goal. Preserve
any failure directory unchanged until the seed and operation index reproduce.

These campaigns are local evidence. Do not add them to GitHub Actions or CTest
without a separate cost, duration, flakiness, and ownership decision.
