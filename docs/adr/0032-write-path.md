# ADR-0032: Write Path

- Status: Accepted
- Date: 2026-09-24

## Context

A write appends a write batch ([ADR-0016](0016-write-batch-format.md)) to the
write-ahead log ([ADR-0018](0018-wal-stream-io.md)), syncs the log when asked,
and inserts the batch into the memtable
([ADR-0017](0017-arena-backed-memtable.md)) under sequence numbers that the
version set allocates ([ADR-0027](0027-version-set.md)). Concurrent writers
queue, and the writer at the front commits the batches of the writers behind
it together, so that one log record and one sync serve many writes.

This implements only the `implement-write-path` DAG node: the writer queue
with group commit, and committing a group to the log and the memtable.
Making room for a write, which switches to a new memtable and log when the
memtable is full and delays writes when level 0 has too many files, belongs to
the flush and engine nodes, and so do the database mutex, the last sequence
that readers see, and the error that stops writes after a failure.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Database writes | Queue concurrent writes under the database mutex, let the front writer make room and commit the group, and give every writer the group's result |
| Committing a group | Number the group's entries from the next sequence, append it to the log, sync the log when asked, and insert it into the memtable |
| Recovery | Insert a replayed batch into a memtable |

No current caller requires pipelined or parallel memtable writes, write
throttling by rate, or two-phase commit.

## Prior art

### Google LevelDB

Adopt `DBImpl::Write` and `BuildBatchGroup`:

- Each writer waits under the database mutex until it is at the front of the
  queue or an earlier writer has committed its batch.
- The front writer makes room, builds a group of its batch and the batches of
  the writers behind it in queue order, and commits the group with the mutex
  released during I/O: it numbers the entries from the last sequence plus
  one, appends one log record, syncs the log if its own write asked for a
  sync, and inserts the entries into the memtable.
- A group stops before a writer that asks for a sync when the front writer
  does not, and before it would exceed 1 MiB, or the front batch's size plus
  128 KiB when the front batch is at most 128 KiB.
- Every writer of the group gets the group's result, the front writer
  removes them from the queue, and the next writer becomes the front.
- A batch without entries is written to the log like any other and takes no
  sequence numbers.

Change:

- **The caller's batch is not changed.** LevelDB sets the sequence in the
  front writer's own batch when it commits it alone. Here the queue commits a
  copy, so writes take a constant batch.
- **The steps are separate from the queue.** LevelDB's `Write` makes room,
  commits, and records errors in one function. Here the queue calls a prepare
  function and a commit function that the engine provides, and
  `PrepareGroup` and `CommitGroup` perform the checks and the log and
  memtable steps, so each is tested on its own.
- **Everything that can fail is checked before the log.** LevelDB can append a
  batch to the log and then fail to insert it into the memtable, whose key
  length limit is 8 bytes shorter than the batch's. Here `PrepareGroup`
  checks the sequence range and every key's length before any I/O, so a
  group that reaches the log always reaches the memtable, and recovery can
  replay every record that a write logged.
- **Exceptions release the queue.** If making room, building the group, or
  committing it throws, the front writer takes the lock back, completes the
  writers of its group with `Aborted`, because their outcome is unknown, and
  wakes the next writer before the exception leaves `Write`.

### RocksDB

RocksDB's write path adds pipelined and parallel memtable writes, write
stalls by rate, two-phase commit, and write-prepared transactions. These are
rejected because no current caller needs them.

## Decision

Add `src/engine/write_path.{h,cc}`:

```cpp
Status InsertBatch(WriteBatchReader& batch, MemTable& memtable);

Status PrepareGroup(EncodedWriteBatch& group, SequenceNumber first_sequence);
Status CommitGroup(const EncodedWriteBatch& group, bool sync, WalWriter& log,
                   MemTable& memtable);

class WriteQueue final {
 public:
  using Prepare = std::function<Status(std::unique_lock<std::mutex>& lock)>;
  using Commit =
      std::function<Status(std::unique_lock<std::mutex>& lock, EncodedWriteBatch& group,
                           bool sync)>;

  WriteQueue(Prepare prepare, Commit commit);

  Status Write(std::unique_lock<std::mutex>& lock, const EncodedWriteBatch& batch, bool sync);
  std::size_t size() const noexcept;
};
```

- `InsertBatch` adds the remaining entries of the batch to the memtable with
  their sequences and returns the memtable's first error.
- `PrepareGroup` sets the group's sequence to `first_sequence`. It returns
  `InvalidArgument`, changing nothing, if an entry would take a sequence
  above `MaxSequenceNumber`, or if a key is longer than the memtable accepts,
  which needs a key over 4 GiB. It performs no I/O. A group fails together,
  so such a batch also fails the writes grouped with it; both cases need
  2^56 writes or a key over 4 GiB.
- `CommitGroup` appends a prepared group to the log, syncs the log if `sync`
  is set, and then inserts the group into the memtable, which cannot fail for
  a prepared group whose sequences the version set allocated. It returns the
  log's first error. The log writer keeps that error, so the log accepts no
  later record. It may throw `std::bad_alloc` from the log writer or the
  memtable. Only the front writer calls it, without the database mutex,
  because the memtable accepts one writer at a time alongside concurrent
  readers.
- `Write` requires `lock` to own the database mutex. It queues the batch and
  waits until it is at the front or its batch has been committed. At the
  front, it calls `prepare`; if that fails, only this writer returns the
  error. Otherwise it builds the group from the writers queued then, as
  LevelDB does, and calls `commit`, whose status every writer of the group
  returns. `prepare` and `commit` are called with the lock held, may release
  it while they wait or perform I/O, and must hold it when they return;
  writers that arrive meanwhile join a later group. After the group, the
  front writer wakes the next writer.
- If `prepare`, building the group, or `commit` throws, the front writer
  takes the lock back if needed, completes the writers already chosen for its
  group with `Aborted`, removes itself, wakes the next writer, and rethrows.
- The group is a batch that the queue owns and reuses; `commit` may change it,
  such as by setting its sequence. It starts from sequence zero whatever
  sequences the callers' batches hold, so appending a batch to it fails only
  if the count overflows, which the size limit prevents.
- `size` returns the number of queued writers, including the one at the
  front, and requires the lock.
- The queue is neither copyable nor movable. `prepare` and `commit` must not
  call `Write`.

The engine's commit function computes the next sequence and calls
`PrepareGroup` with the mutex held; a failure there has written nothing. It
then calls `CommitGroup` with the mutex released. With the mutex held again,
it treats any error or exception from `CommitGroup` as a permanent error that
stops later writes, because the log's state is unknown, and otherwise raises
the last sequence by the group's count, so that readers see the group only
after it is in the memtable, as LevelDB does.

## Explicitly deferred behavior

- Making room for writes, background errors, and the database mutex, which
  belong to the flush and engine nodes.
- Pipelined or parallel memtable writes and rate-based write stalls.

## Validation plan

Unit tests cover:

- `InsertBatch` numbering, values, and deletions.
- `PrepareGroup` and sequence exhaustion, and `CommitGroup` through an
  in-memory file system: the log record and sync, the memtable entries, and a
  failure at every file operation, after which nothing is inserted.
- `WriteQueue` with scripted prepare and commit functions and gates instead
  of sleeps: writers that queue while the front writer prepares join its
  group; a prepare failure affects only the front writer; the sync rule and
  both size limits end a group; batches group whatever sequences they hold;
  every writer of a group returns its status; a writer that arrives while
  `commit` has released the lock waits, gets only its own group's status, and
  leads the next group; exceptions from each step release the queue; and many
  threads whose writes are each committed exactly once in each thread's
  order.
- Recovery replaying through `InsertBatch`.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

No differential helper is needed: the queue's grouping follows LevelDB's rules
directly, and a group's log record is an `EncodedWriteBatch` encoding, which ADR-0016
already compared with LevelDB.

## Consequences

- The engine's writes share one tested group-commit protocol, and a committed
  group reaches the log before the memtable.
- The engine owns making room and error handling around the commit.

## References

- [Google LevelDB `DBImpl::Write`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [RocksDB write path](https://github.com/facebook/rocksdb/blob/main/db/db_impl/db_impl_write.cc)
