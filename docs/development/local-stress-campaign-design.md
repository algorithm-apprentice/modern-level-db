# Local Stress Campaign Orchestration

## Purpose

Add V5 orchestration around the existing V1 through V4 local stress tools.

V5 does not add another database oracle. It owns a deterministic multi-seed
plan, launches one bounded subprocess at a time, preserves every attempt,
publishes resumable state, and ensures that all successful jobs came from the
same source identity.

Long campaigns remain developer-owned and local. V5 adds no scheduled workflow,
GitHub Actions campaign, or CTest stress run. A fast Python contract test may
run in ordinary CI to prevent the orchestrator itself from rotting.

## Entry point

```text
tools/run_local_stress_campaign.py
```

Planned command:

```console
python3 tools/run_local_stress_campaign.py \
  --stress-dir BUILD_DIRECTORY/stress \
  --workspace NEW_OR_RECOGNIZED_CAMPAIGN_DIRECTORY \
  [--seeds 1,17,301] \
  [--tiers v1,v2,v3,v4[,mixed]] \
  [--v1-operations 5000] \
  [--v2-rounds 300] \
  [--v3-rounds 25] \
  [--v3-inflight-probes 3] \
  [--v3-timeout-seconds 30] \
  [--v4-operations 250] \
  [--v4-liveness-writes 2000] \
  [--mixed-threads 32] \
  [--mixed-operations-per-thread 10000] \
  [--mixed-keys 1048576] \
  [--mixed-reopens 10] \
  [--job-timeout-seconds 3600] \
  [--max-duration-seconds 0] \
  [--resume]
```

`--max-duration-seconds 0` means no invocation-wide wall-clock limit. Every
other count/timeout is positive. Seeds are unsigned 64-bit integers, retain
their command-line order, and may not repeat.

The stress directory must contain the current platform's:

```text
modern_leveldb_stress
modern_leveldb_concurrent_stress
modern_leveldb_crash_stress_worker
modern_leveldb_option_fault_stress
modern_leveldb_mixed_stress
```

Windows uses the `.exe` names. V3 is launched through the adjacent maintained
`run_crash_stress.py`; no copied runner is accepted.

## Deterministic plan

For every selected seed, the standard all-tier plan contains:

| Tier | Jobs per seed | Profiles |
|---|---:|---|
| V1 | 3 | None, Snappy, Zstd |
| V2 | 3 | 2/None, 4/Snappy, 8/Zstd |
| V3 | 3 | None, Snappy, Zstd |
| V4 | 1 | complete internal 48-cell/11-fault matrix |
| Mixed | optional 1 | RocksDB-applicable 10/20/45/15/10 profile |

The full default remains 30 V1-through-V4 jobs. Selecting `mixed` adds one job
per seed without changing the identity or ordering of the existing jobs.

Stable job IDs are:

```text
seed-<seed>-v1-<compression>
seed-<seed>-v2-t<threads>-<compression>
seed-<seed>-v3-<compression>
seed-<seed>-v4
seed-<seed>-mixed
```

The plan records exact argv arrays, budgets, expected completion/run artifact
paths, and a versioned schema. It is serialized with sorted JSON keys and a
SHA-256 digest before any job starts.

## Workspace ownership

A new run requires an absent workspace with an existing parent. It creates:

```text
workspace/
  plan.json
  events.jsonl
  state.json
  summary.json
  summary.txt
  jobs/
    000-seed-1-v1-none/
      state.json
      attempts/
        001/
          command.json
          stdout.log
          stderr.log
          result.json
          database/
          artifacts/
```

V4 uses `attempt/workspace/` instead of separate database/artifact paths.

The orchestrator never deletes a workspace, attempt, database, or artifact.
Every retry receives the next monotonically numbered attempt directory.

## Subprocess ownership

Commands are argv arrays and never pass through a shell.

- stdout and stderr stream directly into attempt-owned files;
- only one job runs at a time;
- POSIX launches a new session and terminates the complete process group;
- Windows launches a new process group, sends `CTRL_BREAK_EVENT`, waits a
  bounded grace period, and then kills the direct parent if necessary;
- V1, V2, and V4 create no child process;
- the healthy V3 parent already owns and reaps every worker, and its workers
  inherit the orchestrator-created process group.

The effective job deadline is the smaller of:

- `--job-timeout-seconds`;
- the remaining invocation-wide duration budget, when nonzero.

Timeout, interruption, launch failure, nonzero exit, missing completion, bad
artifact format, and source-identity mismatch are distinct result categories.

`KeyboardInterrupt` terminates the owned process group, records the interrupted
attempt when possible, writes the current summary, and exits with code 130.

## Source identity

Every successful V1 through V4 job has an artifact `run.txt` containing:

```text
revision=<git object>
dirty=<true|false|dirty|clean>
```

The first successful job establishes the campaign identity. Every later
successful job, including one completed before resume, must match it exactly.
Rebuilding only part of the stress suite during a campaign therefore fails
explicitly rather than producing a success-shaped mixed-source summary.
The orchestrator normalizes `false`/`clean` and `true`/`dirty` before comparing
the C++ driver and V3 worker encodings.

## Durable orchestration state

`events.jsonl` is append-only. Every line is flushed and `fsync`ed before the
next transition.

`state.json` and `summary.json` use:

```text
write temporary sibling
flush
fsync
os.replace
best-effort parent-directory fsync where admitted
```

The state records each job as:

```text
pending
running
passed
failed
timed_out
interrupted
```

`completion.txt` is written only when every planned job is `passed`.

## Resume

`--resume` requires an existing recognized workspace.

The newly generated plan must match `plan.json` byte-for-byte except for the
stored digest field. A changed seed list, tier set, workload budget, executable
path, or command is rejected before launching a subprocess.

Resume behavior:

- revalidate every passed job's completion, run format, and source identity;
- skip only passed jobs that still satisfy those checks;
- preserve failed, timed-out, interrupted, or previously running attempts;
- retry the first non-passed job in a new attempt directory;
- continue in original plan order;
- never infer success from exit code alone.

A stale `running` state means the previous orchestrator did not publish a final
transition. It becomes a preserved interrupted attempt on resume.

## Duration exhaustion

The orchestrator checks the invocation-wide deadline before every launch.

If no time remains, it publishes `budget_exhausted`, leaves remaining jobs
pending, writes no completion file, and exits with code 3. Resume may use a new
duration budget without changing the immutable job plan.

If the budget expires during a job, that attempt is timed out through the same
owned-process termination path.

## Result and failure bundle

Each attempt is already its own retained bundle:

- exact argv and working directory;
- start/end monotonic and wall-clock metadata;
- stdout/stderr;
- return code or launch/timeout/interruption category;
- complete native database and stress artifacts, if created.

The campaign summary lists:

- plan digest and campaign source identity;
- total/passed/failed/pending job counts;
- per-tier and per-seed counts/durations;
- current/final status;
- the first blocking job and attempt;
- paths to every retained attempt.

The default policy stops after the first failed job. Resume retries it before
later jobs, keeping causal ordering simple and avoiding a large collection of
known-bad databases.

## Exit codes

| Code | Meaning |
|---:|---|
| 0 | Every planned job passed and `completion.txt` was published |
| 1 | Job, artifact, identity, launch, timeout, or orchestration failure |
| 2 | Invalid arguments, unsafe workspace, missing executable, or plan mismatch |
| 3 | Invocation-wide duration budget exhausted with pending work |
| 130 | User interruption after owned-process containment |

## Fast tests

- deterministic job count, order, IDs, argv arrays, and plan digest;
- seed/tier/budget parsing and duplicate rejection;
- existing/new/resume workspace safety;
- subprocess success, nonzero exit, launch failure, and timeout;
- stdout/stderr streaming into the exact attempt;
- process-group containment with a fake child where the platform permits;
- completion artifact and source-identity validation;
- mixed revision/dirty identity rejection;
- stop-on-first-failure and preserved attempt bundle;
- atomic state/summary publication;
- stale-running conversion and retry attempt numbering;
- passed-job skip only after artifact revalidation;
- plan mismatch rejection before launch;
- duration exhaustion before and during a job;
- completion suppression for every incomplete/failure result.

Tests use short Python fake commands and temporary directories. They do not run
the database stress campaigns.

## Local acceptance

1. Run a two-seed low-budget all-tier campaign.
2. Interrupt one fake/short campaign and resume it.
3. Force one child timeout and confirm no process survives.
4. Force one database job failure, preserve its attempt, repair the cause, and
   resume through completion.
5. Run the standard seeds 1, 17, and 301 with review-appropriate budgets.
6. Confirm that no GitHub workflow or long CTest entry was added.

## Nonclaims

- V5 does not make a finite campaign a proof of database correctness.
- Wall-clock ordering and durations are not deterministic.
- Windows process groups are not a general Job Object sandbox.
- V5 does not recover an attempt whose child still runs outside its owned
  process group.
- Resume validates published artifacts; it does not reconstruct unflushed
  state lost with a killed orchestrator.

## Delivery sequence

1. Commit this design.
2. Add failing planner and plan-schema tests.
3. Implement deterministic planning and CLI validation.
4. Add failing subprocess/timeout/failure-bundle tests.
5. Implement platform process ownership and attempt publication.
6. Add failing resume/state/summary tests.
7. Implement durable resume and duration policy.
8. Add the local CLI, usage documentation, and fast CTest contract.
9. Run Windows and WSL acceptance scenarios.
10. Review process cleanup, path safety, schema stability, and no-CI boundary.
11. Deliver one PR and mark the V1 through V5 roadmap complete.

## V5 implementation record

V5 is implemented through:

- `tools/run_local_stress_campaign.py` as a thin Python 3.9+ CLI and
  `tools/local_stress_campaign.py` as its importable core;
- a canonical 10-job-per-seed V1 through V4 planner with stable IDs, exact argv
  templates, sorted versioned JSON, and SHA-256 plan identity;
- fresh attempt directories with exact commands, direct stdout/stderr files,
  process results, artifact validation, database/output workspaces, and no
  deletion or reuse;
- POSIX session/process-group and Windows process-group ownership with bounded
  graceful termination and kill fallback;
- fsynced event records plus atomic state, per-job state, summary, and
  completion publication;
- source revision/dirty normalization across the C++ drivers and V3 worker;
- exact-plan resume with passed-artifact revalidation, stale-running
  conversion, monotonic attempt numbering, and duration exhaustion.

The implementation review found and fixed:

1. V3 writes worker identity as `clean`/`dirty`, while V1, V2, and V4 write
   `false`/`true`; the orchestrator now normalizes both encodings before
   comparing source identity;
2. relative executable paths made resume depend on the caller's current
   directory; the immutable plan now stores resolved executable paths and the
   default child working directory is the repository root;
3. unconditional POSIX directory `fsync` rejected filesystems that do not
   admit it; state publication now keeps file sync mandatory and treats only
   documented unsupported directory-sync errors as best effort;
4. an initial summary exposed only global counts despite the design requiring
   tier/seed trends; JSON and text summaries now include per-tier, per-seed,
   and total durations and counts;
5. one 1,355-line executable mixed CLI and testable orchestration; a minimal
   entry point now delegates to a separately importable core module;
6. passed-artifact revalidation initially overwrote the attempt's historical
   category; it now records a separate revalidation category and preserves the
   original attempt result.

Validation completed:

```text
Windows Debug unit                  911/911
Windows Release unit                911/911
Windows model/compatibility/crash     18/18
WSL Ubuntu GCC 13 Debug unit        878/878
WSL Ubuntu GCC 13 Release unit      878/878
WSL model/compatibility/crash         10/10
Windows Python campaign contracts     19/19
WSL Python campaign contracts         19/19
Python 3.9 grammar compatibility       pass
documentation validation               pass
```

Local acceptance:

- a real Windows one-seed all-tier plan ran all ten jobs;
- its first V3 attempt completed the database work but exposed the
  `clean`/`false` identity-encoding difference, stopped before later jobs, and
  retained the complete attempt;
- resume skipped the six already validated V1/V2 jobs, retried only that V3 job
  as attempt 002, ran the remaining V3/V4 jobs, and completed 10/10;
- a WSL Ubuntu ext4 one-seed all-tier plan completed 10/10 directly;
- Windows and WSL fake-child timeout contracts both proved process-group
  containment;
- durable tests cover duration exhaustion followed by resume, stale-running
  conversion, tampered passed-artifact retry, plan mismatch rejection, and
  completion suppression.

V5 is complete at this boundary. The full V1 through V5 local stress roadmap is
implemented without adding a hosted long-running campaign.
