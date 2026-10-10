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

// Overlapping level-0 files amplify reads; this count triggers consolidation.
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

// Scores pressure for levels with an output level. Level 0 uses file count;
// deeper levels use bytes relative to a 10 MiB budget growing tenfold per level.
// A score >= 1 is full, and ties choose the shallower level.
[[nodiscard]] CompactionScore ScoreCompaction(const Version& version);

// Chooses size pressure first, otherwise a file with an exhausted seek budget,
// or nothing. A compact pointer rotates size-driven work through the level.
//
// Expands level-0 overlaps transitively and includes user-key boundary files
// plus next-level overlaps, preserving first-source read correctness. Extra
// source files are admitted only without new next-level inputs and below the
// 25-fold target-size work bound. See docs/learning/07-writes-and-compaction.md.
//
// Requires the version's comparator and compact pointers. Any seek candidate
// must be live at its stated level below NumLevels - 1. The target's 25-fold
// must fit in 64 bits.
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
