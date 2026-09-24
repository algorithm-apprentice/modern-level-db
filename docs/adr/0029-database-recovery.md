# ADR-0029: Database Recovery

- Status: Accepted
- Date: 2026-09-24

## Context

Opening a database locks its directory, creates or recovers the version set
([ADR-0027](0027-version-set.md)), and replays the write-ahead logs that the
MANIFEST does not yet cover. Replayed writes go to memtables, which are
written to level-0 tables ([ADR-0028](0028-level0-table-building.md)), and
the database continues with a new, empty log.

This implements only the `implement-recovery` DAG node. Serving reads and
writes, deleting obsolete files, and scheduling compactions after opening
belong to the read-path, write-path, compaction, and engine nodes.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Database opening | Lock the directory, create a missing database when asked, reject an existing one when asked, and recover every write that the logs hold |
| Write path | Receive the new log and its number, and the version set with the recovered last sequence |
| Read path, flush, and compaction | Receive the version set whose current version holds the recovered tables |

No current caller requires reusing the last log, keeping the recovered
memtable, repairing damaged databases, recovery modes, or reporting skipped
records.

## Prior art

### Google LevelDB

Adopt LevelDB's `DB::Open` and `DBImpl::Recover` with default options:

- Lock `LOCK`. A database without `CURRENT` is created if
  `create_if_missing` is set and is otherwise `InvalidArgument`; an existing
  database is `InvalidArgument` if `error_if_exists` is set.
- Every table file of the current version must exist.
- Logs whose number is at least the log number, or equal to the previous log
  number, are replayed in number order. Each record is a write batch that is
  inserted into a memtable, and a memtable whose memory usage exceeds the
  write buffer size, like the last one, is written to a level-0 table.
- A record that the log reader reports as damaged is skipped, and replay
  continues with the next record. The reader never joins fragments across
  damage, so every replayed batch is complete.
- Replayed log numbers are marked used, and the last sequence is raised to
  the last sequence that the replayed batches used; an empty batch shows that
  the sequences before its own were used.
- A new log is created, and one edit records it as the log number, sets the
  previous log number to zero, and adds the tables.
- The default write buffer size is 4 MiB.

Change:

- **Errors are never ignored.** With default options, LevelDB ignores a log
  that it cannot open and errors from inserting batches. Here every file
  system error is returned.
- **Records that no writer produces fail recovery.** LevelDB skips a record
  that passes its checksum but is not a valid write batch, after inserting
  the part of the batch before the error, and does not check sequences. Here
  such a record is `Corruption`, and so is a batch whose sequence is not above
  the last sequence of the batches before it, or is zero, which also rejects
  duplicate entries. LevelDB gives every batch, including an empty one, the
  sequence after the last one used, and a crash cannot produce a record with
  a valid checksum.
- **Only table files satisfy the table check.** LevelDB accepts any file with
  a live table's number, such as a log. Here a live table needs a
  `<number>.ldb` file; `.sst` names are not accepted, as ADR-0020 decided.
- **Log numbers are marked before replay.** LevelDB marks a log's number used
  after replaying it, so a table written during the replay can receive the
  number of a log that the MANIFEST does not record. Here every replayed log
  number is marked used first.
- **Durable creation.** LevelDB syncs neither the new log nor the directory
  before recording the log. Here the new log is synced and the database
  directory is synced before the edit records the log and the tables, as
  [ADR-0011](0011-filesystem-contracts.md) requires; the log stays open for
  the write path, so it is not closed. Creating a database syncs the parent
  of its directory, so that the directory's entry is durable even if an
  earlier attempt created the directory.
- **A missing directory is not created by accident.** LevelDB creates the
  directory even when `create_if_missing` is not set and then reports that
  the database does not exist. Here a missing directory without
  `create_if_missing` is `InvalidArgument`, and nothing is created.

### RocksDB

RocksDB offers four WAL recovery modes. Its default, point-in-time recovery,
stops replaying all logs at the first damaged record, which keeps every
acknowledged write only because RocksDB syncs closed logs whenever it syncs a
newer one. LevelDB closes a log without syncing it, and damage in the middle
of a log can precede synced records, so stopping could discard synced writes
that LevelDB recovers. The modes, log reuse, and recovery of column families
and blob files are rejected because no current caller needs them.

## Decision

Add `src/engine/recovery.{h,cc}`:

```cpp
struct RecoveryOptions {
  bool create_if_missing = false;
  bool error_if_exists = false;
  // Replay writes a memtable to a table once its memory usage exceeds this.
  std::size_t write_buffer_size = 4 * 1024 * 1024;
  TableBuilderOptions table_options;
};

struct RecoveredDatabase {
  std::unique_ptr<FileLock> lock;
  std::unique_ptr<VersionSet> versions;
  std::unique_ptr<WalWriter> log;
  std::uint64_t log_number;
};

Result<RecoveredDatabase> RecoverDatabase(FileSystem& file_system,
                                          const std::filesystem::path& directory,
                                          const InternalKeyComparator& comparator,
                                          const RecoveryOptions& options,
                                          TableCache& table_cache);
```

`RecoverDatabase`:

1. If the directory does not exist, returns `InvalidArgument` unless
   `create_if_missing` is set, and otherwise creates the directory.
2. Locks `LOCK`.
3. If `CURRENT` is missing, returns `InvalidArgument` unless
   `create_if_missing` is set, and otherwise syncs the parent of the
   directory, which is the path without its last component, ignoring a
   trailing separator, or `.` if there is none, and creates the version set.
   Otherwise returns `InvalidArgument` if `error_if_exists` is set, and
   otherwise recovers the version set.
4. Lists the directory. A table of the current version without a
   `<number>.ldb` file is `Corruption`.
5. Selects the logs whose number is at least the log number or equal to the
   previous log number, and marks their numbers used. A log number at or
   above `FileNumberLimit` is `Corruption`.
6. Replays the logs in number order into a memtable. Corruption events are
   skipped. After each record, a memtable whose memory usage exceeds
   `write_buffer_size` is written to a level-0 table with `BuildTable`, and so
   is the last memtable; memtables without entries are not written.
7. Raises the last sequence to the last sequence that the replayed batches
   used, creates the log numbered by `NewFileNumber`, syncs it, and syncs the
   directory.
8. Records one edit with the new log number, previous log number zero, and
   the tables in level 0, and returns the lock, the version set, and the new
   log.

- File system errors and the errors of `VersionSet`, `BuildTable`, and the
  table cache are returned. A complete record that is not a valid write
  batch, a batch whose sequence does not follow the sequences before it, and
  entries that the memtable rejects are `Corruption`.
- If recovery fails, it releases the lock and evicts from the table cache
  every number it allocated for a table, including one whose build failed or
  threw. The files are left for obsolete-file cleanup, and the logs they came
  from stay in place, so recovering again replays them.
- The lock is declared first in `RecoveredDatabase`, so it is released last.
  The file system, the comparator, and the table cache must outlive the
  result, and the table cache must meet `BuildTable`'s requirements. A
  deleted overload rejects a temporary comparator.
- Skipped records are not reported, because Modern LevelDB has no
  information log.

## Explicitly deferred behavior

- Reusing the last log and keeping the recovered memtable.
- Deleting obsolete files, which the engine does after opening.
- Recovery modes, repair, and reporting skipped records.

## Validation plan

Unit tests with an in-memory file system cover:

- Creating a database and its directory, with the parent directory synced,
  also when a retry finds the directory, and rejecting missing directories
  and databases, and existing databases, by option.
- Replaying logs into level-0 tables, including tables split at the write
  buffer size, empty batches, logs below the log number, the previous log,
  and the resulting last sequence, file numbers, operations, and edit.
- Damaged records, which are skipped, truncated tails, and an empty batch
  after damage, which raises the last sequence.
- Records that are not batches, sequences that do not increase, including a
  duplicate after a table split and empty batches, missing tables, and log
  numbers beyond the limit.
- A failure at every file operation of a recovery that replays logs into
  several tables, after which the lock is released, the tables are not
  cached, and recovering again recovers every entry.
- Lock contention between two recoveries.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

A session-only differential helper has unmodified LevelDB write databases
whose logs it does not flush, damages some log records, and compares the
entries that this recovery finds with those that LevelDB recovers from a copy
of each database.

## Consequences

- Opening a database recovers every intact write that its logs hold, as
  LevelDB does, and fails instead of ignoring an error or applying a record
  that no writer produces.
- Opening always starts a new log and records it, writing a new MANIFEST
  after recovery or appending to the one that `Create` wrote.

## References

- [Google LevelDB `DB::Open` and `DBImpl::Recover`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [RocksDB WAL recovery modes](https://github.com/facebook/rocksdb/blob/main/include/rocksdb/options.h)
- [RocksDB syncing of closed logs](https://github.com/facebook/rocksdb/blob/main/db/db_impl/db_impl_write.cc)
