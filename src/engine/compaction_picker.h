#ifndef MODERN_LEVELDB_ENGINE_COMPACTION_PICKER_H_
#define MODERN_LEVELDB_ENGINE_COMPACTION_PICKER_H_

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "format/internal_key.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"

namespace modern_leveldb {

// Level 0 needs compaction once it has this many files, LevelDB's
// kL0_CompactionTrigger.
inline constexpr std::uint32_t Level0CompactionTrigger = 4;

// The level that most needs compaction and how much: at least 1 means that the
// level is full.
struct CompactionScore {
  std::uint32_t level;
  double score;
};

// A file whose seek budget ran out, in the version whose read found it.
struct SeekCompaction {
  std::uint32_t level;
  Version::File file;
};

// A compaction of files of `level` with the files of the next level that
// overlap them. The version holds every file, so the files stay live.
struct Compaction {
  std::uint32_t level;
  std::shared_ptr<const Version> version;
  // The files of `level` and of `level + 1`, each in its level's order.
  std::array<std::vector<Version::File>, 2> inputs;
  // The files of `level + 2` that overlap the inputs, in order.
  std::vector<Version::File> grandparents;
  // The largest internal key of the inputs of `level`, where the level's next
  // size compaction starts.
  InternalKey compact_pointer;
};

// Returns LevelDB's best level and score for levels 0 to NumLevels - 2, as its
// VersionSet::Finalize computes them: level 0 scores its file count divided by
// Level0CompactionTrigger, and every deeper level its total file size divided
// by 10 MiB times ten for each level below 1. A tie goes to the first level.
[[nodiscard]] CompactionScore ScoreCompaction(const Version& version);

// Returns the compaction that LevelDB's VersionSet::PickCompaction picks: a
// size compaction of the best level if its score is at least 1, starting from
// the first file whose largest key follows the level's compact pointer, and
// otherwise a seek compaction starting from its file, or nothing. The inputs
// then grow as LevelDB's do: overlapping level-0 files, boundary files that
// share a user key, the next level's overlapping files, and more files of the
// level if that keeps the next level's inputs and stays below 25 times the
// target file size.
//
// Requires the comparator that orders the version, the version set's compact
// pointers, a seek compaction whose file is in the version at its level, which
// is below NumLevels - 1, and a target file size whose 25-fold fits in 64
// bits.
[[nodiscard]] std::optional<Compaction> PickCompaction(
    std::shared_ptr<const Version> version, const InternalKeyComparator& comparator,
    std::span<const std::optional<InternalKey>, NumLevels> compact_pointers,
    const std::optional<SeekCompaction>& seek_compaction, std::uint64_t target_file_size);

// Returns whether the compaction's one input can move to the next level
// unchanged: nothing in the next level overlaps it, and its grandparents total
// at most ten times the target file size.
[[nodiscard]] bool IsTrivialMove(const Compaction& compaction, std::uint64_t target_file_size);

// Returns an edit that records the compaction's pointer for its level and
// removes every input. A trivial move then adds its input to the next level,
// and a compaction adds its outputs.
[[nodiscard]] VersionEdit CompactionEdit(const Compaction& compaction);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_COMPACTION_PICKER_H_
