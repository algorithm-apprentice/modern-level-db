# RocksDB-Applicable Mixed Stress Workload

## Purpose

Adapt the parts of RocksDB `db_stress` that Modern LevelDB can express through
its current public API, without copying RocksDB implementation code or claiming
full feature parity.

The source audit is pinned to RocksDB commit:

```text
956c7042dab8355127bf95dc72d378435445f867
```

Primary audited files:

```text
db_stress_tool/db_stress_gflags.cc
db_stress_tool/db_stress_test_base.cc
db_stress_tool/batched_ops_stress.cc
db_stress_tool/db_stress_driver.cc
```

This design adds an original model/oracle and uses RocksDB only to select
applicable workload dimensions and baseline defaults.

## Why thread count is insufficient

A 32- or 256-thread Put-only workload is useful liveness evidence, but it does
not approximate `db_stress` correctness coverage. The important composition is:

```text
mixed operations
+ expected state
+ concurrent ownership
+ snapshots/iterators
+ reopen epochs
+ final exact verification
+ separate crash/fault/options tiers
```

The existing checked/fanout concurrent modes remain focused contracts. The
mixed workload is a separate tool rather than another meaning for those modes.

## Applicability matrix

### Core operation mix

RocksDB's audited default percentages total 100:

| RocksDB operation | Default | Modern API | Current evidence | Disposition |
|---|---:|---|---|---|
| Point read | 10% | `Database::Get` | V1 model; bounded V2 read | Implement exact concurrent per-key check |
| Prefix scan | 20% | `Iterator::Seek`/`Next` | Seek scans, not mixed concurrent | Implement exact quiesced snapshot-prefix check |
| Write | 45% | `Database::Put` | V1/fanout writes | Implement concurrent Put with expected state |
| Delete | 15% | `Database::Delete` | V1/checked Delete | Implement concurrent Delete with expected state |
| Delete range | 0% | unavailable | none | Exclude until a public range-delete API exists |
| Iterate | 10% | bidirectional `Iterator` | V1/fanout iterator checks | Implement exact bounded forward/reverse checks |
| Custom operations | 0% | feature-specific | none | Exclude; no generic Modern API contract |

The implemented profile is therefore:

```text
read=10 prefix=20 write=45 delete=15 iterate=10
```

Percentages are fixed and versioned in the first format. Custom percentage
flags are deferred until the default profile and oracle are stable.

### Scale and lifecycle

| RocksDB dimension | Audited default | Modern disposition |
|---|---:|---|
| Application threads | 32 | Default 32; accept any positive representable count |
| Operations per thread | 1,200,000 | Same default; lower local budgets supported |
| Maximum key slots | 1,048,576 | Same sparse logical key-space default |
| Reopens | 10 | Same default; implemented as exception-safe worker epochs |
| No-overwrite selection | 60% | Implement deterministic per-thread fresh-slot preference |
| Column families | 10 | Exclude; Modern has one database key-space |
| Compression default | Snappy | Default Snappy; V4 retains full option coverage |
| Sync writes | off | Mixed profile uses ordinary asynchronous writes |

### Separate existing tiers

These RocksDB dimensions are applicable but already have stronger dedicated
Modern evidence and should not be ambiguously injected into the mixed oracle:

| Dimension | Existing Modern tier |
|---|---|
| Atomic multi-key batch and snapshot consistency | concurrent checked/fanout |
| Native process death and acknowledged recovery | V3 crash stress |
| Named read/write/open/sync/close failures | V4 fault scenarios |
| Compression, mmap, Bloom, buffer, WAL-creation options | V4 option matrix |
| Deterministic power-loss mutation boundaries | extended power-loss tests |
| Cross-implementation compatibility | LevelDB/model compatibility tests |

### Unsupported public APIs

The first mixed profile explicitly excludes:

- Column Family create/drop/consistency;
- SingleDelete, DeleteRange, Merge, TimedPut, PutEntity, and wide columns;
- MultiGet/MultiScan and attribute groups;
- TransactionDB and multi-operation transactions;
- TTL;
- Backup, Checkpoint, secondary/read-only instances;
- external-file ingestion;
- manual Flush, CompactRange, CompactFiles, PromoteL0;
- pause/resume background work and file-deletion controls;
- runtime SetOptions;
- KeyMayExist, properties, approximate sizes, and checksum APIs;
- WAL lock/flush APIs not exposed publicly.

An exclusion is not evidence. When Modern adds one of these public contracts,
the matrix must be revisited before claiming broader RocksDB-applicable
coverage.

## Executable

The default-off local stress build will add:

```text
modern_leveldb_mixed_stress
```

Arguments:

```text
--db NEW_DATABASE_DIRECTORY
--output NEW_ARTIFACT_DIRECTORY
--seed N                         default 301
--threads N                      default 32, positive
--operations-per-thread N        default 1200000
--keys N                         default 1048576
--reopens N                      default 10, less than operations/thread
--compression none|snappy|zstd   default snappy
```

Long defaults remain local-only. Fast contracts use small values.

## Key and value generation

Logical key slots are integers in `[0, keys)`.

The stored key is an eight-byte deterministic permutation of the slot, encoded
in big-endian byte order. This provides:

- one-to-one slot/key mapping;
- binary keys;
- stable comparator order;
- distributed two-byte prefixes suitable for bounded prefix scans.

Values deterministically encode seed, worker, operation index, and a random
token, with lengths selected from 8, 16, and 24 bytes.

For 60% of writes, a worker first selects its next thread-owned slot that is
expected missing. If that partition is exhausted, selection falls back to the
ordinary seeded random key-space. This is a preference, not a promise that 60%
of all completed writes are forever non-overwriting.

## Concurrent expected-state oracle

The model is split into fixed stripes:

```text
stripe = stable_hash(key) % stripe_count
```

Each stripe owns a mutex and map. Point operations hold exactly one stripe:

```text
lock stripe
perform database operation
validate/update model
unlock stripe
```

This deliberately serializes same-key mixed operations. Same-key overlap is
already covered by checked-mode linearizability; mixed mode instead needs an
exact expected state while preserving concurrency across different keys.

All ordinary operations also hold a shared quiescence lock. Exact prefix,
iterator, epoch, and reopen verification takes the exclusive lock.

No model mutation happens before a database write reports success.

## Operation oracles

### Point read

Under the key stripe lock:

- Get the database value.
- Compare exact missing/present bytes with the model.

### Write

Under the key stripe lock:

- generate deterministic binary value;
- Put;
- publish the value into the model only after success.

### Delete

Under the key stripe lock:

- Delete;
- erase the model entry only after success.

### Prefix scan

Under exclusive quiescence:

- acquire one database Snapshot;
- derive a two-byte prefix from a selected logical key;
- seek and collect matching entries, stopping at the first nonmatching key;
- compute the exact matching model entries;
- require byte-for-byte equality.

### Iterate

Under exclusive quiescence and one Snapshot:

- alternate bounded forward and reverse positioning;
- compare up to 64 entries with the exact model range;
- require strict order, no duplicates, exact keys, and exact values.

### Epoch verification and reopen

The total operations are divided into `reopens + 1` worker epochs.

For each epoch:

1. construct exactly `threads` workers behind a cancellable start gate;
2. run each worker's deterministic share;
3. join all workers and propagate the first exception;
4. merge the striped model;
5. require exact forward and reverse database state;
6. close and reopen before the next epoch;
7. require the same exact state after reopen.

Creating a fresh worker cohort per epoch avoids a reusable barrier whose
participants can be stranded by thread-construction or operation failure.

## Determinism and reproduction

The seed fixes:

- each worker's operation kinds;
- logical key slots;
- fresh-slot choices;
- values;
- scan direction and target.

Native scheduling still determines the order of operations that contend on one
stripe, so final state and timing are not required to be byte-identical across
runs. Every failure reports seed, worker, epoch, and operation index.

## Artifacts

| File | Contract |
|---|---|
| `run.txt` | Format, source identity, pinned profile, seed, scale, reopen and option policy |
| `operation-summary.txt` | Attempted/completed counts per operation and verification kind |
| `epoch-summary.tsv` | Per-epoch operations, entries, exact checks, and reopen result |
| `final-state.txt` | Entry count and stable sorted-state fingerprint |
| `completion.txt` | Written only after every epoch and final reopen pass |
| `failure.txt` | Failure context and message |

The tool does not write every operation by default because the aligned default
can exceed 38 million operations. A later bounded breadcrumb facility may be
added without changing the oracle.

## Fast tests

- exact 10/20/45/15/10 selector boundaries;
- deterministic unique key permutation and two-byte prefixes;
- deterministic 8/16/24-byte values;
- fresh-slot preference and exhausted fallback;
- stripe selection and deadlock-free multi-lock ordering;
- point read/write/delete model transitions;
- exact prefix and bounded forward/reverse iterator checks;
- operation-count partitioning across reopen epochs;
- thread-creation cancellation;
- missing/invalid argument and path safety;
- source identity, summary, completion, and failure publication;
- small 1-, 4-, and 32-thread native runs;
- same seed preserves operation counts even when final schedules differ.

## Local acceptance

- 32 threads with at least 10,000 operations per thread and 10 reopens;
- 256 threads with a bounded operations-per-thread budget and at least one
  reopen;
- None, Snappy, and Zstd across separate fresh runs;
- one Unicode/non-BMP database/output path;
- Windows and POSIX native backends;
- preserve every failure directory unchanged.

## Nonclaims

- This is not full RocksDB `db_stress` parity.
- Excluded APIs receive no evidence from this workload.
- Per-key stripe locks intentionally remove same-key overlap.
- Exact scans temporarily quiesce mutations.
- A finite mixed campaign is evidence, not proof of complete correctness.
- The pinned RocksDB workload must be re-audited explicitly before updating the
  source commit or profile format.

## Delivery sequence

1. Commit this audit and design.
2. Add failing selector/key/value/epoch planner tests.
3. Implement deterministic workload planning primitives.
4. Add failing striped-model and operation-oracle tests.
5. Implement the mixed runner and artifacts.
6. Add the optional local executable and usage documentation.
7. Run small contracts, 32-thread baseline, and 256-thread bounded campaign.
8. Run Windows/WSL suites and review every exclusion/nonclaim.
9. Deliver one PR without a hosted long campaign.

## Implementation record

The RocksDB-applicable mixed workload is implemented through:

- `modern_leveldb_mixed_stress`, built only by the existing default-off local
  stress option;
- the pinned 10/20/45/15/10 operation selector and 60-percent fresh-slot
  preference;
- deterministic eight-byte binary key permutation, two-byte prefixes, and
  8/16/24-byte binary values;
- 256 striped exact-state maps that serialize one logical key while preserving
  concurrency across keys;
- exact point-read validation under the key stripe;
- quiesced Snapshot prefix and bounded forward/reverse iterator validation;
- exception-safe worker cohorts across reopen epochs;
- exact forward/reverse verification before every reopen and immediately after
  every reopen;
- stable summaries/fingerprints and an optional V5 `mixed` tier with the same
  timeout, failure-bundle, source-identity, and resume policy.

The implementation review found and fixed:

1. an initial prefix/iterator oracle copied the complete model for 30 percent
   of operations, which was unusable at the aligned 38.4-million-operation
   default; the key's first byte now selects its model stripe, and bounded
   iterator expectations traverse only the target and adjacent stripes;
2. the initial `fresh_writes` name implied a stronger guarantee than the
   key-space can provide after contention or exhaustion; artifacts now report
   deterministic `fresh_write_plans`;
3. mixed mode initially sat outside campaign ownership; an optional V5 tier
   now provides multi-seed plans, timeout containment, retained attempts, and
   exact-plan resume without changing the original 30-job default;
4. state fingerprint logic would otherwise have been duplicated; the existing
   V4 golden fingerprint was moved into shared stress support and retained its
   exact vectors.

Validation completed:

```text
Windows Debug unit                  924/924
Windows Release unit                924/924
Windows model/compatibility/crash     18/18
WSL Ubuntu GCC 13 Debug unit        891/891
WSL Ubuntu GCC 13 Release unit      891/891
WSL model/compatibility/crash         10/10
focused mixed/V4 fingerprint          12/12
Python campaign contracts             19/19
clang-format                           pass
documentation validation               pass
```

Local campaigns:

| Platform | Threads | Operations/thread | Reopens | Compression | Completed operations | Final entries |
|---|---:|---:|---:|---|---:|---:|
| Windows smoke | 4 | 1000 | 2 | None | 4000 | 982 |
| Windows Unicode/non-BMP | 32 | 10000 | 10 | Snappy | 320000 | 136425 |
| Windows | 256 | 500 | 1 | Zstd | 128000 | 50858 |
| WSL Ubuntu ext4 | 32 | 2000 | 2 | None | 64000 | 28513 |

A real V5 `mixed`-only campaign also completed with one retained job,
validated source identity, summary aggregation, and campaign completion.

This delivery broadens workload composition for the currently expressible API;
it does not convert the exclusions above into tested behavior or full RocksDB
parity.
