# Local Database Stress Testing

## Purpose

Add a reproducible local stress driver that continuously compares a native
Modern LevelDB database with an independent ordered-map model.

The first version is deliberately single-threaded. It establishes the reusable
operation generator, oracle, native lifecycle, artifact, and reproduction
contracts before adding concurrency and process failure. Single-threaded V1 is
the first layer of a larger stress program, not the final completeness target.

Long-running campaigns remain local. This design adds no GitHub Actions job,
scheduled workflow, or required stress-test check.

## Existing evidence and the remaining gap

The repository already has:

- fixed-seed native model tests;
- Modern/LevelDB/model differential and cross-open tests;
- a bounded stateful in-memory database fuzzer;
- deterministic power-loss enumeration over one fixed trace;
- owned native process-crash tests.

Those tiers intentionally answer different questions. None is a standalone
driver that can run an arbitrary number of native operations, preserve its
exact seed and attempted operation stream, periodically reopen and verify the
database, and leave a self-contained failure directory.

V1 supplies that missing composition layer without weakening or replacing the
existing evidence.

## Phased roadmap

### V1: deterministic single-thread native stress

- One foreground operation at a time through the public API.
- Real admitted native filesystem backend.
- Independent ordered-map and snapshot-state oracle.
- Seeded Put, Delete, atomic batch, Get, seek/scan, Snapshot, release, and
  reopen operations.
- Small write buffer so long runs traverse WAL rotation, flush, and
  compaction.
- Versioned operation log written before each attempted operation.
- Final reopen and full forward/reverse state verification.
- Preserved run metadata, operation log, final state, and completion/failure
  evidence.
- Local-only optional build target and presets.

### V2: concurrent API stress

- Multiple readers and writers.
- Explicit history recording with operation invocation/completion boundaries.
- A defined oracle for the public consistency contract rather than comparing
  threads directly with an unsynchronized map.
- Deterministic scheduling hooks for selected interleavings plus randomized
  context-switch campaigns.
- Snapshot, iterator, close, and child-lifetime combinations.

### V3: native process crash and restart

- Parent-owned child process and database directory.
- Operation journal and acknowledged-prefix protocol independent of database
  files.
- Abrupt child termination and bounded restart verification.
- Exact distinction between synchronous acknowledged writes, asynchronous
  writes, and an in-flight atomic batch.
- Windows and POSIX process ownership implemented separately where native
  mechanisms differ.

### V4: option and fault matrix

- Seeded selection from a deliberately small supported option set.
- Copied/mapped reads, compression, Bloom filters, write-buffer sizes, and
  durability settings.
- Injected read/write/open/sync/close failures through existing internal test
  seams.
- Separate campaigns for ordinary correctness, fault recovery, and liveness so
  injected errors do not make the oracle ambiguous.

### V5: campaign orchestration

- Multi-seed local runner with duration limits and retained failure bundles.
- Optional developer-owned nightly execution outside repository CI.
- Trend summaries and corpus promotion for every discovered reproducer.

Advancing phases requires the preceding artifact and reproduction contract to
remain stable. Do not combine V2 through V5 into the first implementation.

## V1 build and repository boundary

Add:

```cmake
MODERN_LEVELDB_BUILD_STRESS=OFF
```

The option is default-off and requires an admitted native backend. It builds
`modern_leveldb_stress` but registers no CTest and changes no workflow.

Local release presets:

```text
local-stress
windows-local-stress
```

Normal unit CI may compile shared stress support and run small deterministic
contract tests. It must not run a long campaign. This keeps the local tool from
silently rotting without adding material hosted runtime.

The stress implementation lives outside `src` and `include`; it is test
infrastructure, not a public library API or production dependency.

## Shared deterministic workload

Refactor `tests/support/database_model.h` so its operation generation and
oracle no longer depend on GoogleTest:

- retain the current operation distribution and seeded `mt19937_64` sequence;
- represent each generated step as an owning `ModelOperation`;
- expose an observer invoked before the operation is attempted;
- throw a diagnostic containing seed and operation index on mismatch;
- retain the existing `ModernClient` and multi-client differential use.

The existing model and compatibility tests must continue to use this shared
workload. The stress driver must not copy a second random operation generator.

Fast tests establish:

- identical seeds generate identical owning operation records;
- different seeds diverge;
- operation fields remain within their key, value, batch, and snapshot bounds;
- the observer sees every operation in order;
- mismatches report the seed and operation index;
- existing fixed-seed model and differential tests still pass.

## V1 command

```console
modern_leveldb_stress \
  --db DISPOSABLE_DATABASE_DIRECTORY \
  --output NEW_ARTIFACT_DIRECTORY \
  [--seed N] \
  [--operations N] \
  [--compression none|snappy|zstd]
```

Defaults:

```text
seed         301
operations   100000
compression  none
```

`--db` and `--output` are required native paths. Both must be absent, distinct,
and non-nested. Their parents must already exist. The tool never opens an
existing database, overwrites an artifact directory, or recursively deletes
either path.

Windows receives native wide arguments. POSIX retains native path bytes.
Option names, numbers, and compression values are ASCII.

Exit status:

| Code | Meaning |
|---:|---|
| 0 | Requested operations and final verification completed |
| 1 | Database, model, filesystem, or artifact failure |
| 2 | Invalid command line or unsafe paths |

## V1 database policy

The driver uses:

- `create_if_missing=true`;
- `error_if_exists=true` for the first open so a path created after validation
  is still rejected;
- an internal client that disables `error_if_exists` only for its own later
  reopen operations;
- `write_buffer_size=64 KiB`;
- `bloom_bits_per_key=10`;
- selected block compression;
- Windows weak-namespace consent because that is the admitted native contract;
- default mapped-read policy.

The operation key space stays bounded while values vary through 0 to 2 KiB.
This permits long histories and repeated physical maintenance without
unbounded live logical state.

The workload releases snapshots before reopen. Every reopen immediately scans
the full latest state before later operations can hide a lost update. The run
ends with another reopen and both forward and reverse verification.

## Artifacts and reproduction

The output directory contains:

| File | Contract |
|---|---|
| `run.txt` | Version, seed, requested operations, compression, and fixed engine policy |
| `operations.log` | Versioned tab-separated operation records, flushed before each attempt |
| `final-state.txt` | Sorted hex-encoded key/value state after final reopen |
| `completion.txt` | Written only after full success |
| `failure.txt` | Failure category/message when the process can still report it |

Binary keys and values are hex encoded. Artifacts do not depend on locale,
JSON libraries, implementation pointer values, wall-clock timestamps, or
unordered iteration.

The reproduction identity is:

```text
stress format version + source revision + seed + operation count
+ compression + fixed policy
```

V1 does not replay `operations.log`; the seed regenerates the same stream.
The explicit log still records the final attempted operation if a failure
occurs and protects diagnosis if the generator changes in a future format
version.

An artifact write error is a test failure. Failure-reporting errors must be
combined with, not silently replace, the original database/model error.

## Unit and integration tests

Fast unit coverage:

- required and duplicate arguments;
- numeric overflow, zero operations, and unknown compression/options;
- native path preservation;
- existing, equal, and nested path rejection;
- deterministic operation descriptions;
- operation-log and final-state escaping;
- same-seed runs produce identical semantic artifacts;
- completion appears only after successful final reopen and verification.

The run test uses small temporary native directories and a bounded operation
count. It is a driver contract test, not the long campaign.

Local acceptance campaigns run at least:

```text
none    seed 1
snappy seed 17
zstd   seed 301
```

with enough operations to force repeated rotation, flush, compaction, snapshot,
and reopen activity.

## Safety and nonclaims

- Use only disposable directories.
- The tool never modifies an existing database.
- External modification of the database during a run remains unsupported.
- V1 has no concurrency correctness claim.
- V1 has no process-crash or power-loss claim.
- V1 does not randomize options or inject I/O faults.
- Completing any finite campaign is evidence, not proof of complete database
  correctness.
- No GitHub billing or runtime is added by a stress campaign in this phase.

## Delivery sequence

1. Commit this design.
2. Add failing workload-core tests, then extract the gtest-free generator.
3. Add failing driver/parser/artifact tests.
4. Implement the optional executable and local presets.
5. Add maintained local usage and safety documentation.
6. Run focused tests and the three local acceptance campaigns.
7. Review the full diff for deterministic reproduction, native path handling,
   no existing-data mutation, and clean future phase boundaries.
8. Open one accumulated PR; do not add or modify a GitHub stress workflow.

## V1 implementation record

V1 is implemented as designed:

- `tests/support/database_model.h` now owns a GoogleTest-free
  `ModelWorkload`, owning `ModelOperation` records, an observer seam, and
  seed/operation diagnostic context shared by model, differential, and stress
  callers;
- `stress/stress.cc` provides argument, path-safety, native model execution,
  operation-log, final-state, completion, and failure contracts;
- `modern_leveldb_stress` is built only through the default-off local stress
  option and presets;
- build-time source revision/dirty identity is refreshed before each stress
  target build;
- no CTest or GitHub workflow runs a stress campaign.

The implementation review found and fixed:

1. a configure-time provenance design that could become stale after later
   source edits; identity generation now runs before every stress target build;
2. a time-of-check/time-of-use window between absent-path validation and
   database creation; the first open now uses `error_if_exists=true`, while
   only the driver's own later reopens permit an existing database;
3. unsafe paths initially reporting runtime failure instead of command/safety
   failure; they now produce exit code 2;
4. a GCC signed-character warning in binary artifact encoding that MSVC did not
   diagnose.

Fast driver tests cover parser duplicates/bounds, existing/equal/nested/missing
paths, deterministic semantic artifacts, attempted-operation retention,
completion suppression after failure, native Unicode paths, source identity,
and the local-only usage boundary.

Local validation completed:

```text
Windows Debug                       879/879
Windows Release                     879/879
Windows model/compatibility/crash    18/18
focused stress/model contracts       10/10
GCC strict object compilation        pass
documentation routing tests          17/17
documentation contract tests         13/13
documentation validation             pass
```

Three native Windows campaigns completed 5,000 operations each:

| Compression | Seed | Final entries |
|---|---:|---:|
| None | 1 | 18 |
| Snappy | 17 | 17 |
| Zstd | 301 | 19 |

The None campaign exercised all eight V1 operation kinds and left ten table
files, one active WAL, `CURRENT`, and a later MANIFEST, demonstrating repeated
physical maintenance and reopen activity rather than a memory-only trace.

V2 remains a separate design task. It must define a concurrent history oracle
and synchronization strategy before adding threads; wrapping the current model
in one mutex would serialize the answer and would not validate the public
concurrency contract.
