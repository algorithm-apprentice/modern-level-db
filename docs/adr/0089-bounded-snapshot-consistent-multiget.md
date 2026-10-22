# ADR-0089: Bounded Snapshot-Consistent MultiGet

- Status: Accepted
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
contract. The header/ADR-0061 amendments, decisive schedules and independent
implementation review remain delivery requirements.
