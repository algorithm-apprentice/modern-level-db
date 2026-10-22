# ADR-0089: Bounded Snapshot-Consistent MultiGet

- Status: Implemented
- Date: 2026-10-22

## Context

Independent concurrent Get calls are already supported. Storage-path
measurements showed a benefit from multiple outstanding reads, not from
renaming a synchronous call or spawning a thread for each key.

The current maintenance BackgroundExecutor can be serial and must keep its
queue/closing obligations. It is not a read-concurrency facility.
RandomAccessFile already permits positioned reads with independent output
buffers, so native and injected backends have the required synchronous
concurrency boundary.

This is the batch-read topic under
[ADR-0085](0085-reviewed-read-optimization-adoption.md).

## Decision

Add a synchronous, bounded MultiGet API. It returns only after all work and
workers have finished. It does not add a coroutine dependency, detached
tasks, a persistent service, direct-I/O defaults or asynchronous resource
lifetimes.

Individual Get, its owning/reused-output alternatives and its concurrency
contract remain unchanged.

### Public shape

Define a reusable alias for the existing owning read result:
`Result<optional<vector<byte>>>`. Its absence/value/error meanings stay the
same; an engaged empty vector is a stored empty value.

Add MultiGetOptions with the existing ReadOptions and max_concurrency,
default four, accepted range one through sixteen. A MultiGet takes a borrowed
span of key ByteViews and returns `Result<vector<OwningReadResult>>`.
Output order and count exactly match the input; duplicates and binary/empty
keys are valid.

The outer Result reports preparation errors, including moved-from database,
invalid concurrency or invalid/foreign/moved snapshot. Each vector entry
reports that key's I/O/corruption/value/deletion result independently.
An ordinary error for one key does not turn other keys into missing values
or stop the batch.

Validate configuration and a supplied snapshot even for an empty batch.
An otherwise valid empty batch returns an empty vector without creating
workers or a new snapshot registration. Concurrency one executes without
creating worker threads.

Inputs must remain alive and unchanged until the call returns. As with Get,
moving/destroying the public Database handle must not race with its use.
Existing snapshot-handle obligations remain in force.

### One read sequence

Capture a shared DatabaseState owner at entry. Prepare read options once.
A supplied snapshot's registration is retained for the complete operation.
Without a supplied snapshot, create one registration at the batch boundary.

Every key uses the same registered sequence. Current source Versions may
advance between keys, but MVCC/compaction must preserve that registered
sequence's values. Do not approximate a batch snapshot by reading the last
sequence separately for each worker.

The captured DatabaseState retains the engine and runtime services until
all workers and snapshot registration have been released. Release the
registration after joining workers, before the state owner disappears.

The temporary registration is intentionally visible in state inspection.
On delivery, broaden DatabaseStateSnapshot's `snapshot_count` and
`oldest_snapshot_sequence` comments and ADR-0061 from explicit public snapshot
handles to all active registered read-sequence pins, including an implicit
MultiGet registration. The engine already reports this registry. Do not hide
the batch pin from compaction accounting or claim the old explicit-only
observation contract remains unchanged.

Workers call the existing engine Get with the prepared sequence and their
own key output. Do not duplicate memtable/table/tombstone lookup logic or
repeatedly validate a public Snapshot pointer in worker threads.

### Bounded workers and publication

Allocate the complete result storage before starting work. Use
`min(key_count, max_concurrency)` workers with disjoint deterministic index
partitions. Vector capacity/size stay fixed while workers write distinct
non-bool elements. No worker pushes into or resizes the shared container.

All workers use independent value buffers. No shared scratch value, iterator,
file position or exception slot is reused concurrently. File positioning,
table/cache synchronization and error behavior remain the established Get
implementation's responsibility.

Join every worker before inspecting/publishing results. Worker creation and
joining are part of the public batch call and its measured cost; do not hide
that cost outside the public API benchmark.

No background read may outlive a failed or successful MultiGet call.
Do not schedule work onto the maintenance executor or wait for reads while
holding the engine mutex.

The worker bound is per call, not a database-wide admission limit or an I/O
deadline. Simultaneous batches multiply worker costs. Output storage is
proportional to key count plus all returned values; the concurrency bound
does not bound returned bytes or retained MVCC history.

### Exception boundary

The existing Result model does not catch allocation or user callback
exceptions. Keep that contract instead of converting exceptions into an
empty optional or success-shaped partial batch.

Capture an exception that crosses a worker boundary, stop assigning further
keys, join every worker, and rethrow on the calling thread. Preserve the
original exception rather than manufacturing an unrelated Io error.
In-flight synchronous file calls still finish normally; bounded concurrency
does not promise an I/O cancellation deadline.

If worker construction fails after some workers started, stop remaining
assignments, join the started workers and propagate the construction
exception. Result storage and captured state must remain alive during this
cleanup. An incompletely filled vector is never returned as success.
The construction exception takes precedence if a worker also throws during
that failed startup. When startup completes, select a captured worker
exception deterministically by worker index after all joins.

Ordinary per-key Result errors remain results, not exceptions and not a
reason to cancel independent keys.

## Verification

- Values, missing/deleted keys, empty values/keys, binary keys, duplicates
  and exact result order/count under concurrency one/four/sixteen.
- Explicit, implicit and foreign/moved snapshot cases; writes and compaction
  during a coordinated batch still expose one read sequence.
- Pause between keys after the previous key's ReadSources is gone; overwrite
  or delete, flush and rewrite through compaction, then verify the later key
  still observes the batch sequence.
- No workers/snapshot registration for a valid empty batch; invalid options
  remain errors even for empty input.
- Moved-from Database behavior and retained runtime/comparator owners.
- A coordinated injected backend measures outstanding positioned reads:
  more than one can overlap, never more than the declared bound, with
  separate destination buffers.
- Per-key Io/Corruption remains distinguishable from absence, while other
  keys still return their correct values.
- Worker callback/allocation exceptions and partial worker-start failure:
  all workers joined, no post-return buffer/service access, original
  exception observed by the caller.
- Fail construction after workers started while another read is blocked.
  Verify stop-before-join, construction-exception precedence, simultaneous
  worker exceptions and caller non-return before blocked work completes.
  Require another writer/GetState call to make progress while cleanup waits,
  proving the joins do not retain the engine mutex.
- Observe registration counts during success, per-key errors, exceptions and
  failed startup, and require restoration afterward.
- Exclude mmap/cache hits in the coordinated read-overlap test; check distinct
  destinations and caller-thread execution at concurrency one.
- Existing Get, iterator, snapshot and writer/background contracts remain.
- Fixed logical work comparing ordinary concurrent Get and the complete
  batch API, including worker/result setup, with memory/cache and scalar
  controls. Do not equate queue-depth throughput with single-key latency.

## Default and scope limits

The new API is explicit; existing Get does not create read workers.
Its default concurrency is four within the stated upper bound.
Users can request one for sequential execution or small batches.

Do not claim that a warm, one-key batch should be faster. Per-batch worker
startup and owned output storage have costs. A reusable pool, filesystem
MultiRead/coalescing, zero-copy results and asynchronous API can be future
independent designs if real workloads justify them.

Obtain an independent architecture persona review before implementation and
a different independent implementation persona review before publication.
Update API/concurrency documentation with the exact meanings above.

Independent design review resolved the observable state-inspection change
from implicit registrations and clarified per-call resource bounds and
startup/worker exception precedence. Re-review approved this concrete
contract. A different independent implementation persona reviewed the concrete
implementation, decisive schedules and common completion/cleanup join loop.
The public state comments and ADR-0061 now include implicit registered pins.

## Implementation and admission evidence

The public facade validates once and delegates to the private batch scheduler.
An implicit registration spans the entire operation; workers call the ordinary
engine Get with its retained sequence. Result slots are fixed before startup,
partitions are strided and disjoint, and the caller is one of the bounded
readers. Four readers therefore use the caller plus three created threads.

Six public-only cases preserve the public include fence. Ten internal cases
exercise actual non-mapped positioned-read overlap and distinct destinations,
history preservation after the old SST is physically removed, actual backend
Io/Corruption translation, worker `bad_alloc`, ordinary per-key errors,
deterministic exception selection and partial-start failure. The partial-start
schedule checks both a throwing in-flight read and a successful in-flight read
that must not begin its next assigned key after cancellation. A writer and
state inspection make progress while the failed call waits to join.

The first ordinary CI coverage report exposed two evidence gaps: fake scheduler
Result errors did not exercise the real engine-error adapter, and throwing
in-flight reads did not exercise the next-assignment stop check. Both were
repaired with the actual backend/worker schedules above, without production
changes, exclusions or weakened gates.

### Complete public API measurements

The sealed production revision is
`1a1754196b42f38e945e0a28c84d1b7dad87c4ae`, with source tree
`1ba0786431a787ac0ffbbf494493e1e852893fb4`. The later coverage repair changes
tests only; the measured production source and public headers are unchanged.
The independently verified campaign retains all 156 accepted fresh processes
and every negative observation.

The closed fixture contains one million records in 47 persisted-full-filter
SSTs. Each main process performs one million fixed-permutation Gets, with
batch size 1,024 where applicable, six pair-balanced rounds, and one/four
physical-core affinity. All data is checked against the independent seeded
generator before and after timing. Logical 8/512 MiB budgets deduct the same
1,252,208-byte selected filter payload in every arm.

Worker construction/joining, temporary registration, owning result/value
allocation and destruction remain inside public API timing. The persistent
four-reader control creates four threads once per complete million-key pass;
MultiGet creates its three worker threads for each batch. No maintenance work
runs during timing. Buffered and mapped arms have a warmed OS page cache:
these numbers are not cold-media latency or direct-I/O evidence.

Positive percentages mean lower wall time per key for four-reader MultiGet.
Every listed main comparison against either serial control is faster in all
six pairs; every comparison against persistent concurrent Get is slower in all
six pairs.

| Read backend | Logical budget | Versus owning serial Get | Versus reused-output serial Get | Versus persistent four-reader owning Get |
|---|---:|---:|---:|---:|
| Buffered | 8 MiB | +48.37% | +48.65% | -15.04% |
| Mapped | 8 MiB | +45.13% | +44.39% | -37.69% |
| Buffered | 512 MiB | +44.66% | +42.81% | -44.52% |
| Mapped | 512 MiB | +45.25% | +42.68% | -41.57% |

Real scalar-dispatch controls retain the same direction: +47.38% and +43.44%
versus owning serial Get for buffered 8 MiB and mapped 512 MiB respectively,
but -14.99% and -47.92% versus persistent four-reader Get. The actual linked
runtime/scalar leaf identity is verified, rather than inferred from build names.

The mapped 512 MiB single-key control performs 200,000 fixed-prefix queries
with one key per call. MultiGet is 7.24% slower in all six pairs. It creates no
worker in that case; registration and owning batch packaging still have costs.

This admits an explicit snapshot-consistent batch API, not a faster replacement
for every Get or an implicit read pool. Applications already managing persistent
concurrent readers can retain that approach; a persistent pool/coalesced-I/O
design would require its own evidence. Existing Get and its defaults are
unchanged.

Retained artifacts are under
`build\read-adoption-20261022\multiget-production`: sealed inputs/source/library,
actual public driver, dispatch proof, raw command/stdout/result records, report
and independently recomputed verification. The first coverage artifact is
retained separately under `multiget-ci-coverage`.
