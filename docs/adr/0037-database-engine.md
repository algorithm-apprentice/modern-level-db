# ADR-0037: Database Engine

- Status: Accepted
- Date: 2026-09-24

## Context

Every part of a database now exists on its own: recovery
([ADR-0029](0029-database-recovery.md)), point reads
([ADR-0030](0030-point-reads.md)), iterators ([ADR-0031](0031-iterators.md)),
group commit ([ADR-0032](0032-write-path.md)), flushes
([ADR-0033](0033-memtable-flush.md)), compaction picking and running
([ADR-0034](0034-compaction-picking.md), [ADR-0035](0035-running-compactions.md)),
and seek statistics ([ADR-0036](0036-seek-statistics.md)). The engine owns
the state that joins them: the database mutex, the memtables and the log, the
version set, snapshots, background work, obsolete-file cleanup, and the error
that stops writes. Those ADRs left each of these obligations to it.

This ADR designs the whole engine. It is implemented by two nodes, which
splits the `implement-db-engine` DAG node as ADR-0028, ADR-0030, and ADR-0034
split theirs:

- `implement-db-engine`: opening and closing, writes and making room for
  them, point reads, iterators, snapshots, background flushes, obsolete-file
  cleanup, and background errors. It keeps its dependencies except the
  compaction-picking, compaction, and seek-statistics nodes.
- `implement-db-compactions`: background compactions and trivial moves, the
  level-0 write slowdown and stop, and the seek charges of reads and iterator
  samples. It depends on `implement-db-engine`,
  `implement-compaction-picking`, `implement-compaction`, and
  `implement-seek-statistics`; `implement-public-api` depends on it.

Until the second node lands, level 0 grows without bound, which only the
engine's own tests exercise.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Public API | Open and close a database; write batches with optional sync; read keys and iterate at the latest state or a snapshot; take and release snapshots |
| Tests and the compatibility harness | Flush the memtable and wait for background work deterministically, with an injected file system, executor, and clock |

No current caller requires manual compactions, properties, approximate
sizes, repair, destroying databases, or several processes sharing one.

## Prior art

### Google LevelDB

Adopt `DBImpl`:

- **Options** are sanitized when a database opens: `max_open_files` to 74
  through 50,000, with ten kept for other files; `write_buffer_size` to 64 KiB
  through 1 GiB; `max_file_size` to 1 MiB through 1 GiB; and `block_size` to
  1 KiB through 4 MiB. Without a block cache, the database owns an 8 MiB one.
- **Opening** recovers the database, starts an empty memtable, removes
  obsolete files, and schedules background work if any is needed.
- **Writes** queue in `DBImpl::Write`. The front writer makes room, then
  numbers its group from the last sequence plus one, writes and optionally
  syncs the log and inserts the group into the memtable with the mutex
  released, and raises the last sequence by the group's count.
- **Making room** returns the background error if there is one; waits one
  millisecond once per write when level 0 has at least 8 files
  (`kL0_SlowdownWritesTrigger`); returns while the memtable uses at most
  `write_buffer_size`; waits for background work while an immutable memtable
  exists or level 0 has at least 12 files (`kL0_StopWritesTrigger`); and
  otherwise switches to a new log and memtable: it allocates a log number,
  creates the log, closes the old log, recording a background error if that
  fails, makes the memtable immutable, starts a new one, and schedules
  background work.
- **Background work** runs one task at a time. A task is scheduled unless one
  is already scheduled, the database is closing, a background error exists,
  or there is neither an immutable memtable nor a compaction to do. The task
  flushes the immutable memtable if there is one, and otherwise runs the
  compaction that `PickCompaction` picks, moving a trivial one; it then
  removes obsolete files, schedules again, and wakes waiting writers. A
  compaction flushes a pending immutable memtable between entries.
- **Reads** capture the memtables and the current version at the snapshot's
  sequence, or the last sequence, and read them with the mutex released; a
  read's seek charge is applied to the version it read. Iterators get a
  seed from a counter, and their samples are charged to the current
  version.
- **Snapshots** record the last sequence; the oldest one, or the last
  sequence if there is none, is a compaction's smallest snapshot.
- **Obsolete-file cleanup** is skipped after a background error. Otherwise it
  keeps logs at or after the log number and the previous log number,
  MANIFESTs at or after the current one, tables and temporary files that a
  live version or a pending output holds, and every other file; it evicts
  removed tables from the table cache and removes the files with the mutex
  released, ignoring errors.
- **Background errors** are sticky: the first one is kept, wakes waiting
  writers, and fails every later write.
- **Closing** marks the database as closing and waits until no background
  task is scheduled.

Change:

- **The version set is used with the mutex held.** LevelDB releases the
  mutex while it writes the MANIFEST, relying on the single background
  thread. Here `LogAndApply` runs with the mutex held, as
  [ADR-0027](0027-version-set.md) requires, so MANIFEST writes briefly block
  foreground reads and writes.
- **The new log's directory entry is synced** before the log accepts writes,
  as ADR-0033 requires; a failure returns the error to the writer.
- **No file-number reuse.** When a new log cannot be created, LevelDB returns
  its number for reuse. Here the number stays used, which is harmless below
  `FileNumberLimit`.
- **Every commit failure stops writes.** LevelDB records a background error
  only for a failed sync; ADR-0032 records one for any failed or throwing
  commit, since the log's state is then unknown.
- **Injected dependencies.** The file system, the background executor, and
  the clock of the slowdown are options, so tests control them.
- **Deterministic waits.** `FlushMemTable` makes room with force, as
  LevelDB's `TEST_CompactMemTable` writes a batch-less writer, and waits
  until the immutable memtable it made is flushed; `WaitForBackgroundWork`
  waits until no background task is scheduled, as LevelDB's tests wait.
  Both return the background error if there is one.
- **A forced writer never joins a group.** LevelDB's `BuildBatchGroup` can
  take a batch-less writer into another writer's group, which then skips its
  forced switch. Here the queue ends a group before a forced writer, so the
  switch always happens.
- **Fallible scheduling.** LevelDB's `Env::Schedule` cannot fail. Here
  `BackgroundExecutor::Schedule` returns a status; a rejected task records a
  background error.
- **Removed features.** Manual compactions, properties, approximate sizes,
  info logs, `paranoid_checks`, and `reuse_logs` are rejected because no
  current caller needs them.

### RocksDB

RocksDB's `DBImpl` adds column families, several background threads,
pipelined and unordered writes, write stalls by rate, and a separate flush
queue. These are rejected because no current caller needs them.

## Decision

Add `src/engine/database.{h,cc}`:

```cpp
struct DatabaseOptions {
  const Comparator* comparator = &BytewiseComparator();
  bool create_if_missing = false;
  bool error_if_exists = false;
  std::size_t write_buffer_size = 4 << 20;
  std::uint64_t max_file_size = 2 << 20;
  std::size_t max_open_files = 1000;
  TableBuilderOptions table_options{};
  BlockCache* block_cache = nullptr;
  FileSystem* file_system = nullptr;
  BackgroundExecutor* executor = nullptr;
  Clock* clock = nullptr;
};

struct DatabaseReadOptions {
  std::optional<SequenceNumber> snapshot;
  bool fill_cache = true;
};

class Database final {
 public:
  static Result<std::unique_ptr<Database>> Open(const DatabaseOptions& options,
                                                std::filesystem::path directory);
  ~Database();

  Status Write(const WriteBatch& batch, bool sync);
  Result<std::optional<std::vector<std::byte>>> Get(ByteView key,
                                                    const DatabaseReadOptions& options = {});
  std::unique_ptr<DbIterator> NewIterator(const DatabaseReadOptions& options = {});
  SequenceNumber GetSnapshot();
  void ReleaseSnapshot(SequenceNumber snapshot);

  Status FlushMemTable();
  Status WaitForBackgroundWork();
};
```

- `Open` sanitizes the options as LevelDB does, uses the POSIX file system, a
  serial executor, and the system clock where none is given, and recovers
  the database with `RecoverDatabase`. The comparator, the block cache, the
  file system, the executor, and the clock must outlive the database. Where
  no POSIX backend exists, as on Windows under
  [ADR-0011](0011-filesystem-contracts.md), `Open` without a file system
  returns `NotSupported`.
- `Write` commits the batch through the `WriteQueue`: its prepare function
  makes room as described, and its commit function numbers the group with
  `PrepareGroup`, commits it with `CommitGroup` without the mutex, records a
  background error if that fails or throws, and otherwise raises the last
  sequence. An empty batch writes a log record and raises nothing.
- `Get` and `NewIterator` read at the snapshot, which `GetSnapshot` must have
  returned and `ReleaseSnapshot` not yet released, or at the last sequence.
  An iterator must be destroyed before its database, since its samples call
  it and its tables live in its table cache.
- `GetSnapshot` records and returns the last sequence; snapshots are counted,
  so each call needs its own `ReleaseSnapshot`, and two at one sequence stay
  until both are released.
- Rotation happens only at the front of the write queue, so no commit is
  writing the log or the memtable that it replaces. Extending ADR-0032's
  queue, `WriteQueue::Force` queues a writer without a batch that calls the
  prepare function with force set, completes alone, and ends any group
  before it; the prepare function takes that flag. `FlushMemTable` forces,
  which switches even an empty memtable as LevelDB does, and waits until the
  engine no longer holds the immutable memtable that the switch made.
- A switch allocates the new log writer and memtable before it syncs the
  directory, closes the old log, or changes any state, so a failure or an
  allocation that throws leaves the old log and memtable in place.
- Scheduling follows LevelDB's conditions, with the mutex held: the engine
  marks a task as scheduled, then calls `Schedule`, which must queue the task
  without running it. If `Schedule` fails or throws, the engine clears the
  mark, records the error, or `Aborted` for an exception, as the background
  error, and wakes every waiter; if that happens while `Open` schedules its
  first task, `Open` returns the error. An
  executor must eventually run every task it accepted while the database is
  open. Tests that run tasks by hand run them from another thread whenever a
  database call waits for background work.
- Each task clears the mark when it ends, schedules again if needed, and
  wakes every waiter. Background work runs one task at a time, and each task
  does one flush or one compaction, as LevelDB's does.
- The flush follows ADR-0033: it allocates the table number and protects it
  with the mutex held, calls `FlushMemTable` on the immutable memtable with
  the mutex released, and then applies the edit. On success it stops
  protecting the number, drops the immutable memtable, wakes every waiter,
  and removes obsolete files. On failure it records the background error
  first, which stops cleanup, and then stops protecting the number.
- Two atomic flags mirror the state that code without the mutex checks: one
  says the database is closing, and one says an immutable memtable exists.
  The mutex still guards the state itself.
- The compaction follows ADR-0034 and ADR-0035: it picks from the current
  version with `SeekStatistics::FileToCompact`, applies a trivial move's edit
  at once, and otherwise runs the compaction with hooks. A task finds a
  compaction whenever it finds no immutable memtable, since the need that
  scheduled it remains: only background work installs versions or drops a
  seek record. The smallest snapshot is the oldest one, or the last sequence
  if there is none. The output-number hook allocates, protects, and records
  each number with the mutex held. The entry hook returns `Aborted` once the
  closing flag is set; when the immutable flag is set, it takes the mutex,
  flushes the immutable memtable, which only background work drops, and so
  wakes the writers that wait for it before the compaction continues. If a
  background error exists after that flush, such as its own failure, the
  hook returns it, which stops the compaction. A compaction that finishes its
  entries after the closing flag is set returns `Aborted` before its edit is
  applied, as LevelDB's check after its loop does. The recorded numbers are
  released after the edit is applied or a background error is recorded, and
  the input iterator and the compaction are destroyed before obsolete-file
  cleanup. A trivial move makes no file obsolete, so it runs no cleanup, as
  in LevelDB.
- Making room adds LevelDB's level-0 triggers, `Level0SlowdownWritesTrigger`
  (8) and `Level0StopWritesTrigger` (12): a writer that is not forced sleeps
  one millisecond through the clock with the mutex released, once per write,
  while level 0 has at least 8 files, and a writer that needs a switch waits
  for background work while level 0 has at least 12.
- Seek charges follow ADR-0036, and `SeekStatistics::Retain` runs after
  every version the engine installs. `Get` charges its read's version with
  the mutex held after the read; an iterator's samples charge the version
  that is current when they arrive; and iterator seeds count from 1, as
  LevelDB's `++seed_` does.
- Obsolete-file cleanup follows LevelDB's rules above.
- `~Database` sets the closing flag, closes the log, which no write can use
  any longer, ignoring an error as LevelDB's destructor does, waits until no
  background task is scheduled, and then releases the directory lock last.
  Closing the log first also gives tests a file operation that shows the
  closing flag is set.

The first node implements everything except compactions, the level-0
slowdown and stop, and seek charges; its background tasks only flush. The
second node adds them without changing the first node's behavior otherwise.

## Explicitly deferred behavior

- The public RAII API and its option and status types, which belong to the
  `implement-public-api` node.
- Compression, which belongs to the `implement-compression` node.

## Validation plan

Unit tests with the in-memory file system and an executor that the tests run
by hand cover, in the first node:

- Option sanitizing, and opening new, existing, missing, and locked
  databases, including recovery of writes from the logs.
- Writes and reads of values and deletions, empty batches, sync writes, and
  concurrent writers.
- Rotation when the memtable fills: the new log synced in the directory
  before it accepts writes, writers waiting while an immutable memtable
  exists, a new log that cannot be created, and an old log that cannot be
  closed.
- Flushes through `FlushMemTable` and through a full memtable, reads of the
  immutable memtable and of tables, and a flush that fails. A
  `FlushMemTable` that starts while a commit has released the mutex waits
  for the commit, and the flushed table holds the whole group.
- `WriteQueue::Force`: it waits for the front, completes alone, and ends a
  group before it.
- Snapshots in reads and iterators, counted snapshots at one sequence, and
  iterators that span the memtables and tables.
- Obsolete-file cleanup of old logs, MANIFESTs, and tables, and its absence
  after a background error; a failed commit or sync that stops later writes.
- A rejected or throwing `Schedule` when the memtable switches, and a
  rejected one when a task schedules again. Opening schedules nothing until
  the second node, since recovery leaves no immutable memtable.
- Without a POSIX backend, `Open` without a file system returns
  `NotSupported`; with one, a database owns its file system, executor, and
  block cache when given none.
- Closing while background work is pending, and reopening afterward.

The second node adds tests of compactions and trivial moves that background
work runs, the slowdown and the stop of writes, compaction failures, a failed
flush inside a compaction, a rejected `Schedule` while opening, seek
compactions from read charges and iterator samples, and closing during a
compaction and after its last entry. A flush lands on level 0 only when it
overlaps level 0 or 1, so tests either flush after warm-up flushes that fill
levels 2 and 1, or craft the version with `VersionSet` and `BuildTable`, and
they read the layout back from the MANIFEST. Level-0 files come from writes
that switch the memtable before a queued task runs; a forced flush and a
close wait for their log's close before the queued task runs; and every test
runs the queued tasks before it closes. Its compaction tests hold snapshots
and check point reads and new iterators at them afterward: a compaction
keeps what the oldest live snapshot reads, moves on to the next oldest once
it is released, uses the last sequence once none is left, and keeps entries
for two snapshots at one sequence until both are released; reads at a
released snapshot's sequence show what a compaction dropped. A gated
compaction shows that a writer waiting for an immutable memtable proceeds
after the flush inside the compaction while the compaction stays paused.
Each node covers every line and branch of its code, as required by
[ADR-0019](0019-test-coverage-policy.md).

A differential helper in the second node has unmodified Google LevelDB and
Modern LevelDB apply the same random writes and reads, including reopening
each other's databases, and compares every read.

## Consequences

- The public API can wrap one engine class.
- Level 0 is unbounded until the second node lands.

## References

- [Google LevelDB `DBImpl`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [RocksDB `DBImpl`](https://github.com/facebook/rocksdb/blob/main/db/db_impl/db_impl.cc)
