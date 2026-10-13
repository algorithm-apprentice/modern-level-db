# Concurrent Database Stress Testing

## Purpose

Add the V2 local concurrent stress layer on top of the merged single-thread
driver.

The public `Database` supports concurrent calls, but a shared `std::map`
protected by one mutex is not a sufficient oracle: it would impose an external
serialization order that the database never observed and could hide ordering
bugs. V2 uses two bounded, explicit concurrency contracts instead:

1. same-key register histories checked for linearizability;
2. disjoint writer partitions checked for atomic batch and snapshot/iterator
   consistency.

The long campaign remains local. V2 adds no GitHub Actions schedule or CTest
campaign.

## Scope and nonclaims

V2 verifies:

- overlapping Put, Delete, atomic-batch update, and Get operations on one key
  admit a sequential history that respects real-time precedence;
- acknowledged disjoint writer batches are all present after quiescence and
  reopen;
- each snapshot observes both keys of one writer's batch at the same
  generation;
- iterators created from a snapshot retain that registration after the public
  Snapshot handle is destroyed;
- shared Database calls complete without races, deadlocks, ownership failure,
  or malformed iteration;
- every failure bundle contains the actual recorded history and seed.

V2 does not prove:

- unbounded linearizability for every database operation;
- serializable transactions;
- concurrent use, movement, or destruction of the same Snapshot, Iterator,
  WriteBatch, or Database handle;
- process-crash or power-loss behavior;
- randomized option or injected-I/O-failure behavior.

Those boundaries remain V3 and V4 work.

## Build and command

The existing default-off `MODERN_LEVELDB_BUILD_STRESS` option builds a second
local executable:

```text
modern_leveldb_concurrent_stress
```

Command:

```console
modern_leveldb_concurrent_stress \
  --db DISPOSABLE_DATABASE_DIRECTORY \
  --output NEW_ARTIFACT_DIRECTORY \
  [--seed N] \
  [--threads N] \
  [--rounds N] \
  [--compression none|snappy|zstd]
```

Defaults:

```text
seed         301
threads      4
rounds       1000
compression  none
```

Threads are limited to 2 through 8. The checker has factorial worst-case
search, and each linearizability round adds one final read after the worker
operations. Keeping the bound explicit prevents a local stress option from
silently turning into an unbounded model checker.

The command reuses V1's absent/distinct/non-nested path validation, build
identity, compression policy, and disposable-data requirements.

## Phase A: same-key linearizability windows

One long-lived worker thread exists per configured stress thread. A coordinator
and all workers share start and finish barriers.

For each round:

1. a seeded generator assigns every worker one operation on `hot-key`;
2. the operation is one of Put, Delete, Get, or a two-record batch whose final
   effect is Put;
3. values encode the round, worker, and operation identity;
4. every worker records a unique invocation tick, performs optional seeded
   `yield` calls, executes the public API call, then records a completion tick;
5. the coordinator performs one final Get after all workers complete;
6. the bounded checker searches for a sequential order consistent with:
   - every operation's response;
   - `A.complete < B.invoke` implies A precedes B;
   - the register value carried from the preceding round;
7. one valid witness order and final value are retained.

The final Get is not counted as a worker operation. Its interval occurs after
all worker completions, so it constrains the actual quiescent state and removes
otherwise ambiguous final-register outcomes.

### History representation

```cpp
enum class RegisterOperationKind { Store, Delete, Read };

struct RegisterOperation {
    std::size_t id;
    RegisterOperationKind kind;
    std::optional<std::string> value;
    std::optional<std::string> observed;
    std::uint64_t invoke;
    std::uint64_t complete;
};
```

Store uses `value`, Delete clears the register, and Read uses `observed`.
Ticks are process-local monotonic event numbers, not timestamps.

The checker returns the witness order. An invalid history records every
operation before failing the campaign.

### Checker tests

Unit tests cover:

- a simple sequential history;
- overlapping writes with either legal final value;
- reads that select one legal overlap order;
- real-time precedence that makes the same read illegal;
- delete/missing transitions;
- an ambiguous valid history requiring backtracking;
- malformed intervals and duplicate IDs;
- bounded operation-count rejection;
- witness replay producing the reported final state.

## Phase B: batch/snapshot/iterator lifetime

The configured threads are divided into at least one writer and one reader.
Writers own disjoint partitions:

```text
partition/<writer>/a
partition/<writer>/b
```

For each generation, one writer atomically puts the same encoded generation
value into both keys through a private WriteBatch. Writers never share or
mutate one batch concurrently.

Readers repeatedly:

1. acquire a private Snapshot;
2. Get both keys of every writer using that snapshot;
3. require each pair to be both missing or both present with the same valid
   writer/generation value;
4. create an Iterator using the same snapshot;
5. destroy the public Snapshot handle;
6. scan the iterator and recheck every observed partition pair.

The writer publishes an `issued_generation` before calling Write. A snapshot
may observe that generation even before the writer thread records completion,
so readers validate against issued, not completed, generations. Final
quiescent verification uses acknowledged completed generations.

After all threads join:

- no snapshot or iterator remains;
- the database is reopened;
- every writer pair must equal its final acknowledged generation;
- a full iterator scan must contain exactly the hot key when present plus the
  expected partition keys.

## Scheduling and shutdown

V2 does not use sleeps for correctness.

- Barriers establish round boundaries.
- Seeded small `yield` counts perturb worker scheduling without defining the
  oracle.
- The first worker exception is retained.
- Every worker still reaches the finish barrier before the coordinator
  propagates failure, preventing stranded threads.
- Snapshot-phase threads use a shared stop flag and join before reopen.
- Database destruction never races an accepted operation.

Unit tests use small round counts and repeat selected seeds. Local acceptance
uses larger runs.

## Artifacts

The output directory contains:

| File | Contract |
|---|---|
| `run.txt` | Concurrent format, source identity, seed, threads, rounds, compression, and fixed policy |
| `linearizability.log` | Per-round initial state, operation intervals/results, final read, and witness order |
| `snapshot-summary.txt` | Writer generations, reader checks, iterator checks, and final partition state |
| `final-state.txt` | Final reopened sorted database state |
| `completion.txt` | Written only after both phases succeed |
| `failure.txt` | Failure message when reporting remains possible |

Unlike V1, complete concurrent artifacts are not expected to be byte-identical
for the same seed because native scheduling changes intervals. The seed fixes
operations and yield counts; the history records the schedule that actually
occurred.

## Delivery sequence

1. Commit this design.
2. Add failing bounded-checker unit histories; implement the checker.
3. Extract shared V1/V2 path, artifact, compression, and build-identity support
   instead of copying it.
4. Add failing concurrent runner tests.
5. Implement Phase A and its artifacts.
6. Implement Phase B and final reopen verification.
7. Update local usage documentation.
8. Run repeated local campaigns across thread counts, seeds, and compression.
9. Review synchronization, failure containment, oracle soundness, and
   nonclaims.
10. Deliver one accumulated PR without a hosted long-running campaign.
