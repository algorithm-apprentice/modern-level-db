#include "engine/compaction_picker.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

using Files = std::vector<Version::File>;

// LevelDB's ExpandedCompactionByteSizeLimit and MaxGrandParentOverlapBytes,
// relative to the target file size.
constexpr std::uint64_t ExpandedCompactionFactor = 25;
constexpr std::uint64_t GrandparentOverlapFactor = 10;

void Expect(const Status& status) noexcept {
  assert(status.has_value());
  static_cast<void>(status);
}

// LevelDB's MaxBytesForLevel for levels 1 and deeper.
double MaxBytesForLevel(std::uint32_t level) {
  double bytes = 10. * 1048576.0;
  for (; level > 1; --level) {
    bytes *= 10;
  }
  return bytes;
}

std::uint64_t TotalFileSize(std::span<const Version::File> files) {
  std::uint64_t total = 0;
  for (const Version::File& file : files) {
    total += file->file_size;
  }
  return total;
}

// The smallest and largest internal keys of files that the version holds.
struct KeyRange {
  const InternalKey* smallest;
  const InternalKey* largest;
};

void Widen(const InternalKeyComparator& comparator, std::span<const Version::File> files,
           KeyRange& range) {
  for (const Version::File& file : files) {
    if (comparator.Compare(file->smallest, *range.smallest) < 0) {
      range.smallest = &file->smallest;
    }
    if (comparator.Compare(file->largest, *range.largest) > 0) {
      range.largest = &file->largest;
    }
  }
}

// Requires at least one file.
KeyRange RangeOf(const InternalKeyComparator& comparator, std::span<const Version::File> files) {
  KeyRange range{.smallest = &files.front()->smallest, .largest = &files.front()->largest};
  Widen(comparator, files.subspan(1), range);
  return range;
}

// Returns the level's files that hold a user key of the range, both ends
// included, as LevelDB's Version::GetOverlappingInputs does. Level-0 files may
// overlap each other, so one that widens the range restarts the search.
Files OverlappingFiles(const Version& version, std::uint32_t level,
                       const Comparator& user_comparator, const KeyRange& range) {
  ByteView begin = range.smallest->user_key();
  ByteView end = range.largest->user_key();
  const std::span<const Version::File> files = version.files(level);
  Files overlapping;
  for (std::size_t index = 0; index < files.size();) {
    const Version::File& file = files[index++];
    const ByteView file_begin = file->smallest.user_key();
    const ByteView file_end = file->largest.user_key();
    if (user_comparator.Compare(file_end, begin) < 0 ||
        user_comparator.Compare(file_begin, end) > 0) {
      continue;
    }
    overlapping.push_back(file);
    if (level != 0) {
      continue;
    }
    if (user_comparator.Compare(file_begin, begin) < 0) {
      begin = file_begin;
      overlapping.clear();
      index = 0;
    } else if (user_comparator.Compare(file_end, end) > 0) {
      end = file_end;
      overlapping.clear();
      index = 0;
    }
  }
  return overlapping;
}

// Adds the level's files that start after the largest key of the files with
// the same user key, as LevelDB's AddBoundaryInputs does, so that no newer
// entry of a user key stays in the level while an older one moves down. Files
// are sorted by smallest key, so the first file that starts after the largest
// key is the only candidate LevelDB's search for the smallest one can find.
void AddBoundaryInputs(const InternalKeyComparator& comparator,
                       std::span<const Version::File> level_files, Files& files) {
  if (files.empty()) {
    return;
  }
  const InternalKey* largest = RangeOf(comparator, files).largest;
  while (true) {
    const auto next = std::ranges::partition_point(level_files, [&](const Version::File& file) {
      return comparator.Compare(file->smallest, *largest) <= 0;
    });
    if (next == level_files.end() || comparator.user_comparator().Compare(
                                         (*next)->smallest.user_key(), largest->user_key()) != 0) {
      return;
    }
    files.push_back(*next);
    largest = &(*next)->largest;
  }
}

// Returns the level files and the next level's files that a compaction
// starting from the files reads, as LevelDB's SetupOtherInputs chooses them.
Compaction SetupInputs(std::shared_ptr<const Version> version,
                       const InternalKeyComparator& comparator, std::uint32_t level, Files inputs,
                       std::uint64_t target_file_size) {
  const Comparator& user_comparator = comparator.user_comparator();
  AddBoundaryInputs(comparator, version->files(level), inputs);
  KeyRange range = RangeOf(comparator, inputs);
  Files next_inputs = OverlappingFiles(*version, level + 1, user_comparator, range);
  AddBoundaryInputs(comparator, version->files(level + 1), next_inputs);
  KeyRange all = range;
  Widen(comparator, next_inputs, all);

  // Take more files of the level if that keeps the next level's inputs.
  if (!next_inputs.empty()) {
    Files expanded = OverlappingFiles(*version, level, user_comparator, all);
    AddBoundaryInputs(comparator, version->files(level), expanded);
    if (expanded.size() > inputs.size() && TotalFileSize(next_inputs) + TotalFileSize(expanded) <
                                               ExpandedCompactionFactor * target_file_size) {
      const KeyRange expanded_range = RangeOf(comparator, expanded);
      Files expanded_next = OverlappingFiles(*version, level + 1, user_comparator, expanded_range);
      AddBoundaryInputs(comparator, version->files(level + 1), expanded_next);
      if (expanded_next.size() == next_inputs.size()) {
        range = expanded_range;
        inputs = std::move(expanded);
        next_inputs = std::move(expanded_next);
        all = range;
        Widen(comparator, next_inputs, all);
      }
    }
  }

  Files grandparents;
  if (level + 2 < NumLevels) {
    grandparents = OverlappingFiles(*version, level + 2, user_comparator, all);
  }
  InternalKey compact_pointer = *range.largest;
  Compaction compaction{.level = level,
                        .version = std::move(version),
                        .inputs = {std::move(inputs), std::move(next_inputs)},
                        .grandparents = std::move(grandparents),
                        .compact_pointer = std::move(compact_pointer)};
  return compaction;
}

}  // namespace

CompactionScore ScoreCompaction(const Version& version) {
  CompactionScore best{.level = 0, .score = -1};
  for (std::uint32_t level = 0; level + 1 < NumLevels; ++level) {
    const double score = level == 0 ? static_cast<double>(version.files(0).size()) /
                                          static_cast<double>(Level0CompactionTrigger)
                                    : static_cast<double>(TotalFileSize(version.files(level))) /
                                          MaxBytesForLevel(level);
    if (score > best.score) {
      best.level = level;
      best.score = score;
    }
  }
  return best;
}

std::optional<Compaction> PickCompaction(
    std::shared_ptr<const Version> version, const InternalKeyComparator& comparator,
    std::span<const std::optional<InternalKey>, NumLevels> compact_pointers,
    const std::optional<SeekCompaction>& seek_compaction, std::uint64_t target_file_size) {
  const CompactionScore score = ScoreCompaction(*version);
  std::uint32_t level = 0;
  Files inputs;
  if (score.score >= 1) {
    level = score.level;
    // A full level has files. Start after the level's compact pointer, and
    // wrap around past its last file.
    const std::span<const Version::File> files = version->files(level);
    const std::optional<InternalKey>& pointer = compact_pointers[level];
    const auto after = std::ranges::find_if(files, [&](const Version::File& file) {
      return !pointer.has_value() || comparator.Compare(file->largest, *pointer) > 0;
    });
    inputs.push_back(after != files.end() ? *after : files.front());
  } else if (seek_compaction.has_value()) {
    level = seek_compaction->level;
    assert(level + 1 < NumLevels);
    assert(std::ranges::find(version->files(level), seek_compaction->file) !=
           version->files(level).end());
    inputs.push_back(seek_compaction->file);
  } else {
    return std::nullopt;
  }

  if (level == 0) {
    const KeyRange start = RangeOf(comparator, inputs);
    inputs = OverlappingFiles(*version, 0, comparator.user_comparator(), start);
  }
  return SetupInputs(std::move(version), comparator, level, std::move(inputs), target_file_size);
}

bool IsTrivialMove(const Compaction& compaction, std::uint64_t target_file_size) {
  return compaction.inputs[0].size() == 1 && compaction.inputs[1].empty() &&
         TotalFileSize(compaction.grandparents) <= GrandparentOverlapFactor * target_file_size;
}

VersionEdit CompactionEdit(const Compaction& compaction) {
  VersionEdit edit;
  Expect(edit.AddCompactPointer(compaction.level, compaction.compact_pointer));
  for (std::uint32_t which = 0; which < compaction.inputs.size(); ++which) {
    for (const Version::File& file : compaction.inputs[which]) {
      Expect(edit.RemoveFile(compaction.level + which, file->number));
    }
  }
  return edit;
}

}  // namespace modern_leveldb
