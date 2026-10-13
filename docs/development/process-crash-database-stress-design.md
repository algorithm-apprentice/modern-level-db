# Process-Crash Database Stress Testing

## Purpose

Add V3 local process-crash stress after the single-thread and concurrent
in-process layers.

The parent must own the child process, acknowledgement journal, timeout,
termination, recovery verification, and artifacts. The child must never run a
Database destructor after the selected crash boundary.

V3 uses a portable Python parent and one native C++ worker. Python's subprocess
layer owns the platform process/pipe handles and maps `kill()` to the admitted
native abrupt-termination mechanism. The C++ worker owns only database
operations and verification. This avoids copying separate large POSIX and
Win32 process frameworks into the stress code.

Long campaigns remain local. V3 adds no scheduled workflow or CTest crash
campaign.

## Correctness model

### Acknowledged iteration

For journal index `i`, one fresh child:

1. opens the existing database, or creates it for index zero;
2. atomically writes and synchronizes:
   - `crash/latest = value(i)`;
   - `crash/marker/i = committed`;
3. writes exactly `ack<TAB>i<LF>` to stdout and flushes it;
4. waits forever without destroying the Database.

The parent:

1. waits for the exact acknowledgement with a bounded timeout;
2. appends and fsyncs `ack<TAB>i<LF>` to its external journal;
3. kills and reaps the child;
4. starts the worker in verification mode;
5. requires every journal marker and the latest value to be present.

The child emits an acknowledgement only after the sync write returns success.
The parent kills only after that acknowledgement has itself been persisted in
the external journal. Recovery must therefore contain the complete acknowledged
prefix exactly.

Each iteration launches a new process against the same database. Repeated
recovery is part of the workload rather than one final check.

### In-flight atomicity probe

A separate fresh database probe:

1. a short-lived worker creates and cleanly closes an empty baseline database;
2. child emits `ready<TAB>0<LF>`;
3. child opens that existing database and attempts one synchronous two-key batch;
4. parent kills immediately after readiness;
5. verification accepts either:
   - neither key/marker;
   - the complete batch and marker.

One-key or partial-batch visibility is failure. The probe does not call the
attempt acknowledged.

The exact scheduler point after readiness is intentionally uncontrolled. The
oracle accepts the two complete legal states.

## Components

### Native worker

```text
modern_leveldb_crash_stress_worker
```

Modes:

| Mode | Behavior |
|---|---|
| `write-hold` | Sync one indexed batch, emit ack, wait forever |
| `inflight-hold` | Emit ready, attempt one sync batch, wait forever if it returns |
| `verify-prefix` | Read the external journal and verify every acknowledged index |
| `verify-optional` | Verify one in-flight batch is wholly absent or wholly present |
| `initialize` | Create and cleanly close an empty baseline for one in-flight probe |
| `identity` | Print source revision and dirty state |

The worker receives native paths on Windows through `wmain`.

`write-hold` and `inflight-hold` deliberately terminate only through the
parent. Their database handle is kept outside exception-cleanup paths after
open; no normal return or destructor can improve the crash image.

### Python parent

The local parent is `tools/run_crash_stress.py`:

```console
python tools/run_crash_stress.py \
  --worker WORKER_BINARY \
  --db NEW_DATABASE_DIRECTORY \
  --output NEW_ARTIFACT_DIRECTORY \
  [--rounds N] \
  [--seed N] \
  [--compression none|snappy|zstd] \
  [--timeout-seconds N]
```

Defaults:

```text
rounds          100
seed            301
compression     none
timeout-seconds 30
```

The seed determines generated value bytes and the placement of optional
in-flight probes. V3 does not randomize database options beyond compression.

The runner:

- requires absent, distinct, non-nested database/output paths with existing
  parents;
- captures stdout/stderr through owned pipes;
- reads one protocol line through a dedicated reader thread and bounded queue
  wait, avoiding platform-specific nonblocking pipe APIs;
- kills and reaps only the child it started;
- fails if the child exits before the expected record, emits trailing data,
  times out, or cannot be reaped;
- preserves the database and all artifacts on failure;
- never recursively deletes user paths.

## External journal

`acknowledged.log` is parent-owned:

```text
format=modern-leveldb-crash-journal-v1
ack<TAB>0
ack<TAB>1
...
```

The parent appends one line only after validating the child record. It flushes
the Python file object and calls `os.fsync()` before terminating the child.

The worker's `verify-prefix` mode parses the journal independently. It rejects:

- missing or wrong format;
- duplicate, skipped, decreasing, malformed, or trailing fields;
- an index above the requested run;
- missing/incorrect database marker or latest value.

The journal is not a database backup. It is an independent test oracle.

## Values and batches

For index `i`:

```text
crash/latest     -> deterministic value(seed, i)
crash/marker/i   -> committed
```

The value contains a fixed header, seed, index, and deterministic repeated
payload large enough to trigger WAL rotation/flush over a campaign.

Both records are one `WriteBatch` with `sync=true`. Verification uses ordinary
point reads and a final full iterator scan. No internal sequence number is
exposed or assumed.

## Artifacts

| File | Contract |
|---|---|
| `run.txt` | Format, worker identity, seed, rounds, compression, timeout |
| `acknowledged.log` | fsynced parent-owned oracle |
| `process.log` | Launch, protocol, kill, exit, and verification events |
| `inflight.txt` | Optional probe result: absent or complete |
| `completion.txt` | Written only after all iterations and final verification |
| `failure.txt` | Runner failure category/message |

The worker identity must be clean for an admitted evidence run. A dirty worker
is allowed for development but recorded.

## Tests

Fast C++ tests:

- worker argument parsing and native paths;
- deterministic indexed value and batch contents;
- journal parsing and every malformed ordering case;
- prefix verification against a small real native database;
- optional batch verification for absent, complete, and partial states;
- worker protocol formatting.

Fast Python tests use a generated fake worker to cover:

- exact ack/ready parsing;
- timeout and early exit;
- malformed/trailing output;
- journal fsync path;
- owned kill/reap;
- fresh-path rejection;
- success/failure artifact publication;
- identity capture and command construction.

The repository CI may run these bounded contracts. It must not run the local
multi-process crash campaign.

## Local acceptance

Run fresh campaigns for None, Snappy, and Zstd. Each campaign uses at least 25
acknowledged kill/recover iterations and several in-flight probes.

On Windows, include a Unicode database/output path. On POSIX, retain native
path bytes.

Review must confirm no child survives timeout, failure, or normal completion.

## Nonclaims

- `kill()` process termination is not machine power loss.
- The external journal is trusted test infrastructure.
- V3 proves only acknowledged-prefix recovery and whole-batch optionality for
  the selected protocol.
- It does not inject individual filesystem operation failures.
- It does not cover concurrent client calls inside the child.
- It does not make Windows weak namespace durability equivalent to POSIX.

## Delivery sequence

1. Commit this design.
2. Add failing journal/value/verification unit tests.
3. Implement worker core and native entry point.
4. Add failing Python process-runner tests with a fake worker.
5. Implement parent orchestration and artifacts.
6. Run local native crash campaigns and review surviving processes.
7. Update usage documentation and V1–V5 status.
8. Deliver one PR without a hosted crash campaign.
