# Option and Fault Matrix Database Stress Testing

## Purpose

Add V4 local stress coverage for two different questions:

1. does the ordinary public database preserve the same model semantics across
   a bounded option matrix?
2. do selected injected I/O failures preserve error ownership, batch atomicity,
   sticky-stop behavior, and recoverability?

These questions use separate runners and oracles. Randomly continuing after an
injected uncertain write would make the expected state ambiguous and could
mistake intended sticky-stop behavior for a liveness failure.

Long campaigns remain local. V4 adds no GitHub Actions or CTest campaign.

## Native option matrix

The ordinary matrix uses the public `Database` API and real admitted native
filesystem. It runs the complete Cartesian product:

| Dimension | Values |
|---|---|
| Compression | None, Snappy, Zstd |
| File access | platform mapped default, copied |
| Bloom | disabled, 10 bits/key |
| Write buffer | 64 KiB, 256 KiB |
| WAL creation sync | enabled, disabled |

The matrix contains 48 cells. Windows always supplies explicit weak-namespace
consent; disabling WAL-creation sync does not waive that requirement.

Each cell:

- owns a fresh database directory;
- runs the shared seeded model workload;
- uses binary keys/values, batches, snapshots, scans, and reopen;
- ends with another reopen and exact forward/reverse state verification;
- writes one deterministic summary/fingerprint row.

The operation seed is derived from the campaign seed and stable cell identity,
not cell execution order. Reordering the matrix cannot silently change a cell's
workload.

Mapped eligibility is a policy request, not a claim that every table maps.
The matrix verifies public behavior under the option, while native filesystem
tests retain exact mapping/fallback evidence.

## Fault scenarios

Fault scenarios use `DatabaseEngine` with the existing thread-safe
`MemoryFileSystem`, `ManualExecutor`, and `ManualClock`. One fresh database and
filesystem are created per scenario.

| Scenario | Injected boundary | Required result |
|---|---|---|
| Read | first table `read` | Get returns typed error; clearing the hook permits the same read |
| WAL append | next log `append` | write fails and becomes sticky; reopen yields the old complete batch state |
| WAL sync | next log `sync` | write fails and becomes sticky; reopen yields an allowed whole-batch state, never partial |
| Old WAL close | rotation closes old `.log` | rotation fails sticky; reopen yields the old complete batch state |
| Table open | flush output `.ldb` open | background/flush fails sticky; reopen recovers all acknowledged values |
| Table sync | flush output `.ldb` sync | background/flush fails sticky; reopen recovers all acknowledged values from WAL/installed state |
| Table close | flush output `.ldb` close | background/flush fails sticky; reopen recovers all acknowledged values |
| Table directory sync | second runtime database-directory sync | publication fails sticky; reopen recovers all acknowledged values |
| MANIFEST append | runtime MANIFEST append | installation fails sticky; reopen recovers all acknowledged values |
| MANIFEST sync | runtime MANIFEST sync | installation fails sticky; reopen recovers all acknowledged values |
| Scheduling | background executor rejects work | write/flush reports and retains the scheduling failure without stranded work |

Every mutation scenario uses a two-key atomic batch and records the reopened
state as either the complete old or complete new state where uncertainty is
allowed. A one-key result is always failure.

The injected hook fails one named operation without effect, as
`MemoryFileSystem` specifies. Scenarios do not claim arbitrary partial syscall
effects; deterministic power-loss and process-crash tiers cover different
models.

After a sticky failure, the scenario performs only:

- one explicit later write to prove the same error is returned without unsafe
  progress;
- cleanup;
- reopen and recovery verification.

It never treats “keep issuing random writes after an uncertain failure” as a
valid liveness test.

## Liveness scenario

Liveness has no injected failures.

- Use the internal engine, tiny admitted write buffer, ManualExecutor, and
  deterministic operation count.
- Repeatedly write disjoint keys and drain every accepted background task.
- Periodically read and scan.
- Require all tasks to drain, no sticky error, and exact final/reopened model.

This catches scheduling/progress regressions without an error oracle competing
with expected sticky-stop behavior.

## Command

The existing default-off stress option builds:

```text
modern_leveldb_option_fault_stress
```

Planned command:

```console
modern_leveldb_option_fault_stress \
  --workspace NEW_DISPOSABLE_DIRECTORY \
  [--seed N] \
  [--operations N] \
  [--liveness-writes N]
```

Defaults:

```text
seed       301
operations 250
liveness   2000 writes
```

The workspace must be absent and have an existing parent. The runner creates:

```text
workspace/
  databases/
  artifacts/
```

It never opens or deletes an existing workspace.

## Artifacts

| File | Contract |
|---|---|
| `run.txt` | Source identity, seed, operations, matrix version |
| `cells.tsv` | Stable cell ID/options, derived seed, final entries, state fingerprint |
| `faults.tsv` | Scenario, injected operation, first error, sticky check, reopened state |
| `liveness.txt` | Writes, tasks drained, reads/scans, final entries |
| `completion.txt` | Written only after all matrix cells/scenarios succeed |
| `failure.txt` | Failure category/message |

State fingerprints use a repository-owned stable byte hash over sorted
hex-encoded key/value pairs, never `std::hash`.

## Fast tests

- exactly 48 unique option cells;
- every dimension value appears and every Cartesian tuple is present;
- stable cell IDs and seed derivation;
- workspace existing/nested/missing-parent rejection;
- same cell/seed produces the same final state and fingerprint;
- all fault scenarios inject the intended named operation;
- write faults become sticky and suppress later filesystem progress;
- every reopened mutation state is whole-batch old or new;
- read faults are retryable and non-sticky;
- no-fault liveness drains all work;
- deterministic artifacts and completion/failure publication.

Fast tests use small operation counts. The complete native 48-cell campaign
remains local.

## Local acceptance

- Run all 48 native cells on the admitted platform.
- Run every fault scenario at least once.
- Run liveness with at least 2,000 writes and enough background rotations to
  create and compact multiple tables.
- Repeat the full campaign with seeds 1, 17, and 301.
- Preserve every failure workspace unchanged.

## Nonclaims

- The 48 cells are complete only for the explicitly selected option values.
- MemoryFileSystem failures occur without effect at one named operation.
- V4 does not replace real process crash or deterministic power-loss evidence.
- V4 does not inject native kernel/media errors into the owned filesystem.
- Liveness under no fault does not prove availability after arbitrary device
  failure.

## Delivery sequence

1. Commit this design.
2. Add failing option-cell planner/fingerprint tests.
3. Implement native matrix core and bounded contract tests.
4. Add failing fault-scenario tests.
5. Implement named fault and no-fault liveness runners.
6. Add optional local executable and artifacts.
7. Run full local matrix/fault/liveness campaigns.
8. Review oracle separation, sticky behavior, scenario names, and nonclaims.
9. Deliver one PR without a hosted matrix campaign.
