#include "engine/compaction_picker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "base/result_internal.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"

namespace modern_leveldb {
namespace {

using Files = std::vector<Version::File>;

// Bound current input expansion and overlap cost of a future compaction,
// expressed relative to the target output size.
constexpr std::uint64_t ExpandedCompactionFactor = 25;
constexpr std::uint64_t GrandparentOverlapFactor = 10;

// Geometric level capacity keeps each deeper level an order of magnitude larger.
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

// Finds inclusive user-key overlaps. Level 0 is not a disjoint run: widening
// the range can include files skipped earlier, so restart until the closure is stable.
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

// Extends the right edge across files sharing its user key. Otherwise older
// versions left in the shallower level could hide newer versions moved down:
// point reads stop at the first source that decides. Sorted smallest keys make
// the first file after the current edge the only next boundary candidate.
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
        if (next == level_files.end() ||
            comparator.user_comparator().Compare((*next)->smallest.user_key(),
                                                 largest->user_key()) != 0) {
            return;
        }
        files.push_back(*next);
        largest = &(*next)->largest;
    }
}

struct InputExpansion {
    Files inputs;
    Files next_inputs;
    KeyRange range;
};

struct InitialCompaction {
    std::uint32_t level;
    Files inputs;
};

std::optional<InitialCompaction> SelectInitialCompaction(
    const Version& version, const InternalKeyComparator& comparator,
    std::span<const std::optional<InternalKey>, NumLevels> compact_pointers,
    const std::optional<SeekCompaction>& seek_compaction) {
    const CompactionScore score = ScoreCompaction(version);
    if (score.score >= 1) {
        const std::span<const Version::File> files = version.files(score.level);
        const std::optional<InternalKey>& pointer = compact_pointers[score.level];
        const auto after = std::ranges::find_if(files, [&](const Version::File& file) {
            return !pointer.has_value() || comparator.Compare(file->largest, *pointer) > 0;
        });
        Files inputs;
        inputs.push_back(after != files.end() ? *after : files.front());
        return InitialCompaction{.level = score.level, .inputs = std::move(inputs)};
    }
    if (!seek_compaction.has_value()) {
        return std::nullopt;
    }
    Files inputs;
    inputs.push_back(seek_compaction->file);
    return InitialCompaction{.level = seek_compaction->level, .inputs = std::move(inputs)};
}

void CloseLevelZeroInputs(const Version& version, const InternalKeyComparator& comparator,
                          std::uint32_t level, Files& inputs) {
    if (level != 0) {
        return;
    }
    const KeyRange start = RangeOf(comparator, inputs);
    inputs = OverlappingFiles(version, 0, comparator.user_comparator(), start);
}

std::optional<InputExpansion> TryExpandInputs(const Version& version,
                                              const InternalKeyComparator& comparator,
                                              std::uint32_t level,
                                              std::span<const Version::File> inputs,
                                              std::span<const Version::File> next_inputs,
                                              const KeyRange& all, std::uint64_t target_file_size) {
    if (next_inputs.empty()) {
        return std::nullopt;
    }
    Files expanded = OverlappingFiles(version, level, comparator.user_comparator(), all);
    AddBoundaryInputs(comparator, version.files(level), expanded);
    if (!(expanded.size() > inputs.size() && TotalFileSize(next_inputs) + TotalFileSize(expanded) <
                                                 ExpandedCompactionFactor * target_file_size)) {
        return std::nullopt;
    }
    const KeyRange expanded_range = RangeOf(comparator, expanded);
    Files expanded_next =
        OverlappingFiles(version, level + 1, comparator.user_comparator(), expanded_range);
    AddBoundaryInputs(comparator, version.files(level + 1), expanded_next);
    if (expanded_next.size() != next_inputs.size()) {
        return std::nullopt;
    }
    InputExpansion expansion{.inputs = std::move(expanded),
                             .next_inputs = std::move(expanded_next),
                             .range = expanded_range};
    return expansion;
}

void ApplyExpandedInputs(const Version& version, const InternalKeyComparator& comparator,
                         std::uint32_t level, std::uint64_t target_file_size, Files& inputs,
                         Files& next_inputs, KeyRange& range, KeyRange& all) {
    std::optional<InputExpansion> expansion =
        TryExpandInputs(version, comparator, level, inputs, next_inputs, all, target_file_size);
    if (!expansion.has_value()) {
        return;
    }
    range = expansion->range;
    inputs = std::move(expansion->inputs);
    next_inputs = std::move(expansion->next_inputs);
    all = range;
    Widen(comparator, next_inputs, all);
}

Files CollectGrandparents(const Version& version, const Comparator& user_comparator,
                          std::uint32_t level, const KeyRange& range) {
    if (level + 2 >= NumLevels) {
        return {};
    }
    return OverlappingFiles(version, level + 2, user_comparator, range);
}

// Closes the input range over both levels, then records grandparents to bound
// the overlap inherited by the output's next compaction.
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

    ApplyExpandedInputs(*version, comparator, level, target_file_size, inputs, next_inputs, range,
                        all);
    Files grandparents = CollectGrandparents(*version, user_comparator, level, all);
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
    std::optional<InitialCompaction> initial =
        SelectInitialCompaction(*version, comparator, compact_pointers, seek_compaction);
    if (!initial.has_value()) {
        return std::nullopt;
    }
    CloseLevelZeroInputs(*version, comparator, initial->level, initial->inputs);
    return SetupInputs(std::move(version), comparator, initial->level, std::move(initial->inputs),
                       target_file_size);
}

bool IsTrivialMove(const Compaction& compaction, std::uint64_t target_file_size) {
    return compaction.inputs[0].size() == 1 && compaction.inputs[1].empty() &&
           TotalFileSize(compaction.grandparents) <= GrandparentOverlapFactor * target_file_size;
}

VersionEdit CompactionEdit(const Compaction& compaction) {
    VersionEdit edit;
    static_cast<void>(edit.AddCompactPointer(compaction.level, compaction.compact_pointer));
    for (std::uint32_t which = 0; which < compaction.inputs.size(); ++which) {
        for (const Version::File& file : compaction.inputs[which]) {
            static_cast<void>(edit.RemoveFile(compaction.level + which, file->number));
        }
    }
    return edit;
}

}  // namespace modern_leveldb
