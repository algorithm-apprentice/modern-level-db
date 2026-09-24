# ADR-0034: Compaction Picking

- Status: Accepted
- Date: 2026-09-24

## Context

Compaction merges files of one level with the files of the next level that
overlap them, so that level 0 keeps few files and every deeper level stays
within its size. Before a compaction runs, the engine must decide whether a
version needs one and which files it reads: the level's inputs, the next
level's inputs, and the grandparent files that bound its outputs. A single
input file that no next-level file overlaps can move down without being
rewritten.

The `implement-compaction` DAG node held picking, running compactions, and
the seek statistics that [ADR-0030](0030-point-reads.md) and
[ADR-0031](0031-iterators.md) left to the compaction work. This ADR splits it
into three nodes, as [ADR-0028](0028-level0-table-building.md) and ADR-0030
split theirs:

- `implement-compaction-picking`, this node: choosing compactions from a
  version and recording trivial moves. It depends on `implement-version-set`.
- `implement-compaction`: running a compaction into output tables and the
  edit that installs them. It depends on this node, `implement-iterators`,
  `implement-sstable-writer`, `implement-table-cache`, and
  `implement-version-set`.
- `implement-seek-statistics`: the seek budgets that point reads and iterator
  samples charge, which name a file to compact. It depends on this node,
  `implement-read-path`, and `implement-iterators`, and fulfills what
  ADR-0030 and ADR-0031 left to the compaction work.

The engine node depends on all three.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Engine background work | Ask whether the current version needs a compaction, pick one, and install a trivial move directly |
| Running a compaction | Get the input files of both levels, the grandparents, the version that holds them, and the next compact pointer |
| Seek statistics | Turn the file whose seek budget ran out into a compaction when no level is too large |

No current caller requires manual compactions of a key range, other
compaction styles, or several compactions at once.

## Prior art

### Google LevelDB

Adopt `VersionSet::Finalize`, `PickCompaction`, `SetupOtherInputs`,
`AddBoundaryInputs`, `Version::GetOverlappingInputs`, and
`Compaction::IsTrivialMove` and `AddInputDeletions`:

- A version's compaction score is, for level 0, its file count divided by 4
  (`kL0_CompactionTrigger`), and for each level from 1 to 5, its total file
  size divided by 10 MiB times ten for each level below 1. The level with the
  highest score, the first one on a tie, is the one to compact. Level 6 is
  never compacted.
- A size compaction runs if the best score is at least 1. It starts from the
  first file of the level whose largest key follows the level's compact
  pointer, or the level's first file if none does. Otherwise a seek
  compaction starts from the file whose seek budget ran out. Otherwise
  nothing is compacted.
- At level 0, the start is replaced by every level-0 file that overlaps it,
  growing the user-key range with each file that extends it until it stops
  growing.
- Boundary files join the inputs of each level: while a file of the level
  starts after the inputs' largest internal key with the same user key, the
  one with the smallest such key is added. Otherwise a newer entry of a user
  key would stay in the level while an older one moved down.
- The next level's inputs are its files that overlap the level inputs'
  user-key range, with their boundary files.
- If there are next-level inputs, the level's files that overlap the range of
  all inputs, with their boundary files, replace the level inputs if there
  are more of them, their size plus the next-level inputs' size is below 25
  times the target file size, and the next level's files that overlap their
  range, with boundary files, are as many as before.
- The grandparents are the files two levels down that overlap the range of
  all inputs, if that level exists.
- The level's compact pointer becomes the largest internal key of the level
  inputs, and the compaction's edit records it and removes every input.
- A compaction is a trivial move if it has one level input, no next-level
  inputs, and grandparents totaling at most ten times the target file size;
  the file then moves to the next level unchanged.
- Overlap compares user keys, both ends included.

Change:

- **Scores on demand.** LevelDB stores each version's best level and score
  when it installs the version. Here `ScoreCompaction` computes them from the
  version when asked, in the same double-precision arithmetic, so that ties
  and rounding match.
- **The pointer moves with the edit.** LevelDB also advances its in-memory
  compact pointer when it picks, so that a failed compaction retries
  elsewhere. A failed compaction stops all later compactions, as a
  background error does in LevelDB, so here the pointer advances only when
  the edit is applied.
- **The seek file comes from the caller.** LevelDB keeps the file to compact
  in the version. Here the caller passes it, and the seek-statistics node
  keeps it.
- **Bounded ranges only.** LevelDB's overlap query also accepts open ends and
  its `VersionSet::CompactRange` picks manual compactions; no current caller
  compacts a key range by hand.

### RocksDB

RocksDB's leveled picker adds dynamic level sizes, compensated sizes for
deletions, choosing files by overlap ratio or age, intra-level-0 and
parallel subcompactions, and universal and FIFO styles. These are rejected
because no current caller needs them, and LevelDB's choices keep file layouts
comparable with the format oracle's.

## Decision

Add `src/engine/compaction_picker.{h,cc}`:

```cpp
inline constexpr std::uint32_t Level0CompactionTrigger = 4;

struct CompactionScore {
  std::uint32_t level;
  double score;
};

struct SeekCompaction {
  std::uint32_t level;
  Version::File file;
};

struct Compaction {
  std::uint32_t level;
  std::shared_ptr<const Version> version;
  std::array<std::vector<Version::File>, 2> inputs;
  std::vector<Version::File> grandparents;
  InternalKey compact_pointer;
};

CompactionScore ScoreCompaction(const Version& version);

std::optional<Compaction> PickCompaction(
    std::shared_ptr<const Version> version, const InternalKeyComparator& comparator,
    std::span<const std::optional<InternalKey>, NumLevels> compact_pointers,
    const std::optional<SeekCompaction>& seek_compaction, std::uint64_t target_file_size);

bool IsTrivialMove(const Compaction& compaction, std::uint64_t target_file_size);

VersionEdit CompactionEdit(const Compaction& compaction);
```

- `ScoreCompaction` returns LevelDB's best level and score for levels 0 to
  `NumLevels - 2`.
- `PickCompaction` returns LevelDB's compaction for the version, or nothing
  if the best score is below 1 and there is no seek compaction. The
  compaction keeps the version, so its files stay live; `inputs[0]` are files
  of `level` and `inputs[1]` files of `level + 1`, each in the level's order,
  and `grandparents` are files of `level + 2` in order. `compact_pointer` is
  the largest internal key of `inputs[0]`. It requires the comparator that
  orders the version, the version set's compact pointers, a seek compaction
  whose file is in the version at its level, which is below `NumLevels - 1`,
  and a target file size whose 25-fold fits in 64 bits. It asserts that the
  seek compaction's file is in the version.
- A seek compaction belongs to one version, as LevelDB keeps the file to
  compact in the version whose read found it. The seek-statistics node
  updates it with the database mutex held and discards it when that version
  is no longer current, so that a read that finishes after a newer version
  was installed cannot name a file the current version lacks.
- `IsTrivialMove` returns whether the compaction has one level input, no
  next-level inputs, and grandparents totaling at most ten times the target
  file size.
- `CompactionEdit` returns an edit that records the compact pointer for the
  level and removes every input from its level. A trivial move adds the
  level input to the next level; running a compaction adds its outputs.
- LevelDB checks `level + 2 < kNumLevels` before choosing grandparents. That
  is false for level 5, so the check stays.

The engine picks with the database mutex held, from the current version and
the version set's compact pointers. It applies a trivial move's edit at
once, and otherwise runs the compaction.

## Explicitly deferred behavior

- Running compactions, which belongs to the `implement-compaction` node.
- Seek budgets and choosing the file to compact, which belong to the
  `implement-seek-statistics` node.
- Manual compactions of a key range and overlap queries with open ends,
  which serve only them. ADR-0033 left shared overlap queries with open ends
  or level-0 range expansion to the compaction node; this node adds the
  level-0 range expansion for bounded ranges, private to the picker as the
  flush's queries are private to it, and open ends stay outside the DAG
  until a caller needs them.

## Validation plan

Unit tests cover:

- `ScoreCompaction` for level-0 file counts, level sizes against their
  limits, ties, and the last level, which never scores.
- `PickCompaction` for no compaction; size compactions after the compact
  pointer, at its end, and wrapping around; seek compactions and the
  preference for size compactions; level-0 expansion that restarts as the
  range grows; boundary files in both levels; next-level inputs; expansion
  that is taken, and refused for size, at exactly the limit, or because the
  next level would grow; grandparents, and none below level 5; the compact
  pointer; and a reverse comparator.
- `IsTrivialMove` and `CompactionEdit`, including a trivial move applied
  through the version builder.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

An isolated differential helper has unmodified Google LevelDB write fresh
databases without reads, so that it runs no seek compactions, and without
manual compactions, so that every compaction is one it picked. It replays
each MANIFEST with Modern LevelDB's version builder. A record that removes a
file is a compaction or a trivial move; LevelDB picks it after it applies the
previous such record, on its single background thread, and may apply flush
records before and after picking it. So for every such record, some version
from the one after the previous such record up to the one before this record
must give a `PickCompaction` result with exactly the record's removed files
and compact pointer, which is a trivial move exactly when the record moves
its only file down a level.

## Consequences

- The engine and the compaction runner share one tested description of a
  compaction.
- Seek statistics plug in as a caller-provided file.

## References

- [Google LevelDB `VersionSet::PickCompaction` and `SetupOtherInputs`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [Google LevelDB `DBImpl::BackgroundCompaction`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [RocksDB leveled compaction picker](https://github.com/facebook/rocksdb/blob/main/db/compaction/compaction_picker_level.cc)
