# ADR-0035: Running Compactions

- Status: Accepted
- Date: 2026-09-24

## Context

A picked compaction ([ADR-0034](0034-compaction-picking.md)) names input
files in two levels and the grandparent files below them. Running it merges
the inputs in key order ([ADR-0031](0031-iterators.md)), drops the entries
that no snapshot can read, writes the rest to new tables of the next level
([ADR-0024](0024-sstable-writer.md)), and produces the edit that replaces the
inputs with those tables.

This implements only the `implement-compaction` DAG node: the merged input of
a compaction, and running it into verified output tables and the edit that
installs them. Picking compactions, applying edits, protecting output numbers
from obsolete-file cleanup, choosing the smallest snapshot, scheduling, and
recording the error that stops later work belong to the picking and engine
nodes.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Engine background work | Run a picked compaction that is not a trivial move without the database mutex, flush a pending immutable memtable between entries, and get the edit that installs the outputs |

No current caller requires compaction filters, merge operators, range
deletions, several threads for one compaction, or outputs outside the next
level.

## Prior art

### Google LevelDB

Adopt `VersionSet::MakeInputIterator`, `DBImpl::DoCompactionWork`,
`OpenCompactionOutputFile`, `FinishCompactionOutputFile`,
`InstallCompactionResults`, and `Compaction::ShouldStopBefore` and
`IsBaseLevelForKey`:

- The input merges each level-0 input as its own child and the inputs of a
  deeper level as one iterator over their files, reading without filling
  the block cache.
- Before each input entry, the compaction flushes a pending immutable
  memtable, so that writers do not wait for a long compaction, and stops
  when the database closes.
- Before each entry, the grandparent files that end before its key are
  passed; once an entry has been seen, their sizes add up, and when the sum
  exceeds ten times the target file size, it restarts from zero and the
  current output ends before the entry, so that no output overlaps too much
  of the level below the next.
- An entry is dropped if an entry with the same user key came before it with
  a sequence at most the smallest snapshot, because every reader then sees
  that newer entry. A deletion is dropped if its sequence is at most the
  smallest snapshot and no file in the levels below the next one holds its
  user key, because nothing older remains that it hides; the entries it
  shadowed in this compaction are dropped by the first rule. The check
  scans each deeper level once, since keys arrive in order.
- Every other entry goes to the current output, which opens at the first
  such entry with a fresh file number. An output ends once its size reaches
  the target file size, and after the last entry.
- An output is finished, synced, closed, and then opened through the table
  cache to check that it is usable.
- The edit removes every input and adds each output to the next level with
  its first and last keys; it also records the compact pointer.

Change:

- **Corrupt keys stop the compaction.** LevelDB copies an entry whose key is
  not an internal key into its output. Here it is `Corruption`, as for
  database iterators, and the table builder accepts only internal keys, as
  ADR-0024 requires.
- **The directory is synced before the edit.** LevelDB never syncs the
  directory entries of new tables. Here the compaction syncs the directory
  once after its outputs and before it returns an edit that references them,
  as [ADR-0011](0011-filesystem-contracts.md) requires.
- **Engine steps are hooks.** LevelDB's compaction takes the database mutex
  to allocate output numbers and to flush the immutable memtable. Here the
  engine passes a function that returns a protected output number and a
  function that runs before each entry, whose error stops the compaction.
- **The input is an argument.** The compaction reads any internal iterator,
  so tests can script its entries and failures; the engine passes the one
  that `NewCompactionIterator` creates for the compaction.
- **No statistics.** LevelDB records compaction statistics and logs; no
  current caller reads them.

### RocksDB

RocksDB's `CompactionJob` adds subcompactions over key ranges, compaction
filters, merge operators, range deletions, blob files, per-level output
sizes, and output placement by key age. These are rejected because no
current caller needs them.

## Decision

Add `src/engine/compaction.{h,cc}`:

```cpp
struct CompactionOptions {
  TableBuilderOptions table_options{};
  std::uint64_t target_file_size = 2 * 1024 * 1024;
};

struct CompactionHooks {
  std::function<std::uint64_t()> new_file_number;
  std::function<Status()> before_entry;
};

std::unique_ptr<InternalIterator> NewCompactionIterator(
    const Compaction& compaction, TableCache& table_cache,
    const InternalKeyComparator& comparator);

Result<VersionEdit> RunCompaction(FileSystem& file_system,
                                  const std::filesystem::path& directory,
                                  const InternalKeyComparator& comparator,
                                  const CompactionOptions& options, TableCache& table_cache,
                                  const Compaction& compaction, InternalIterator& input,
                                  SequenceNumber smallest_snapshot,
                                  const CompactionHooks& hooks);
```

- `NewCompactionIterator` merges the compaction's inputs as LevelDB's
  `MakeInputIterator` does, reading with `fill_cache` off. Creating it
  performs no I/O. It holds the compaction's version and borrows the
  compaction's input lists, so the compaction must outlive it without
  changing them; the table cache and the comparator must outlive it too. A
  temporary compaction or comparator is rejected at compile time.
- `RunCompaction` positions the input at its first entry and runs LevelDB's
  loop over it with `smallest_snapshot`, calling `before_entry` before each
  entry and `new_file_number` for each output. It returns
  `CompactionEdit(compaction)` with every output added to level
  `level + 1`, in key order.
- It returns the first error of the hooks, the input, the table builder, or a
  file operation, and `Corruption` for an entry whose key is not an internal
  key, without changing any version. Outputs finished or begun before the
  error stay in the directory, and finished ones in the table cache, for
  obsolete-file cleanup, as LevelDB leaves them.
- `CompactionOptions::target_file_size` is LevelDB's `max_file_size`: the
  size at which an output ends, and a tenth of the grandparent bytes after
  which one ends early. The engine clips it as ADR-0033 describes.
- `RunCompaction` touches only its arguments, so the engine calls it without
  the database mutex; the hooks take the mutex themselves.

The engine's compaction computes the smallest snapshot, the oldest live
snapshot's sequence or the last sequence if there is none, and creates the
input with the database mutex held; runs the compaction with the mutex
released; and, with the mutex held again, applies the edit with `LogAndApply`
or records the error that stops later work. It must also:

- Have `new_file_number` allocate a number, protect it from obsolete-file
  cleanup, and record it for the compaction, all with the mutex held, since a
  failed compaction returns no numbers. The recorded numbers stay protected
  until the edit is applied, or until the error that stops later work, which
  also stops obsolete-file cleanup until the database reopens, as ADR-0033
  requires, because a failed `LogAndApply` may leave its edit durable.
- Destroy the input iterator before the compaction, and both before
  obsolete-file cleanup after installing the edit, because the compaction's
  version keeps its inputs live.

## Explicitly deferred behavior

- Scheduling compactions, choosing the smallest snapshot, protecting output
  numbers, applying edits, obsolete-file cleanup, and compaction statistics,
  which belong to the engine node.
- Seek statistics, which belong to the `implement-seek-statistics` node.

## Validation plan

Unit tests cover:

- `NewCompactionIterator` over level-0 inputs that overlap, a deeper level's
  inputs, and both levels, without filling the block cache.
- `RunCompaction` merging both levels into an output with its keys and size,
  and an edit that removes the inputs, adds the outputs, and records the
  pointer; the directory synced once, after the outputs are verified.
- Each drop rule, and entries that a snapshot keeps: older versions below
  and above the smallest snapshot, deletions at and above it, and deletions
  that a file in a level below the next one keeps, including across several
  files of such a level; and a compaction whose entries are all dropped,
  which writes nothing.
- Outputs that end at the target size and outputs that end early for their
  grandparents, including grandparents passed before the first entry, and a
  sum exactly at the limit. The grandparents are passed for every entry
  before it is kept or dropped: a dropped entry that crosses the limit still
  ends the open output, and crossing it while no output is open still
  restarts the sum.
- Hooks: every entry calls `before_entry`, whose error stops the compaction,
  and every output takes a number from `new_file_number`.
- A key that is not an internal key, a failure at every input move, and a
  failure at every file operation.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

An isolated differential helper has unmodified Google LevelDB write fresh
databases through an environment that never removes files, so that every
input survives. Some sessions take one snapshot midway and hold it until the
database closes. LevelDB takes the smallest snapshot when a compaction
starts: that snapshot's sequence once it exists, and otherwise the last
sequence, which is at least every input's sequence. Every comparison with the
smallest snapshot therefore comes out the same with the held snapshot's
sequence, or with the largest sequence in sessions without one, as with the
value LevelDB took. The helper replays each MANIFEST, finds each compaction's
pick as ADR-0034's differential does, runs the compaction with
`RunCompaction` in a separate directory that holds links to the inputs, with
LevelDB's output numbers and that smallest snapshot, and checks that the
edit matches the record and that every output file is byte-for-byte
identical to LevelDB's. If several candidate versions give the pick with
different grandparents, one of them must reproduce the record.

## Consequences

- The engine's compactions are one tested call between its locked steps.
- Output tables match LevelDB's byte for byte, so databases stay comparable
  with the format oracle through compactions.

## References

- [Google LevelDB `DBImpl::DoCompactionWork`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [Google LevelDB `VersionSet::MakeInputIterator` and `Compaction`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [RocksDB `CompactionJob`](https://github.com/facebook/rocksdb/blob/main/db/compaction/compaction_job.cc)
