#include "metadata/version.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace modern_leveldb {
namespace {

constexpr std::uint64_t BytesPerSeek = 16384;
constexpr std::uint64_t MinimumSeeks = 100;

std::int64_t InitialAllowedSeeks(std::uint64_t file_size) noexcept {
    const std::uint64_t seeks = std::max(file_size / BytesPerSeek, MinimumSeeks);
    return static_cast<std::int64_t>(
        std::min(seeks, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
}

bool FileBefore(const InternalKeyComparator& comparator, const Version::File& left,
                const Version::File& right) noexcept {
    const int order = comparator.Compare(left->smallest.encoded(), right->smallest.encoded());
    return order != 0 ? order < 0 : left->number < right->number;
}

Version::File MakeVersionFile(const FileMetadata& metadata) {
    FileMetadata file = metadata;
    file.allowed_seeks = InitialAllowedSeeks(file.file_size);
    return std::make_shared<const FileMetadata>(std::move(file));
}

using LevelCounts = std::array<std::size_t, NumLevels>;

struct PreparedChanges {
    std::array<std::vector<std::uint64_t>, NumLevels> deletions;
    std::array<std::vector<Version::File>, NumLevels> additions;
};

template <typename Records>
LevelCounts CountByLevel(const Records& records) {
    LevelCounts counts{};
    for (const auto& record : records) {
        ++counts[record.level];
    }
    return counts;
}

void ReserveChanges(PreparedChanges& changes, const VersionEdit& edit) {
    const LevelCounts deletion_counts = CountByLevel(edit.deleted_files());
    const LevelCounts addition_counts = CountByLevel(edit.new_files());
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        changes.deletions[level].reserve(deletion_counts[level]);
        changes.additions[level].reserve(addition_counts[level]);
    }
}

void CollectDeletions(PreparedChanges& changes, const VersionEdit& edit) {
    for (const DeletedFile& deleted : edit.deleted_files()) {
        changes.deletions[deleted.level].push_back(deleted.number);
    }
}

void InitializeAdditions(PreparedChanges& changes, const VersionEdit& edit) {
    for (const NewFile& added : edit.new_files()) {
        changes.additions[added.level].push_back(MakeVersionFile(added.file));
    }
}

PreparedChanges PrepareChanges(const VersionEdit& edit) {
    PreparedChanges changes;
    ReserveChanges(changes, edit);
    CollectDeletions(changes, edit);
    InitializeAdditions(changes, edit);
    return changes;
}

}  // namespace

std::span<const Version::File> Version::files(std::uint32_t level) const noexcept {
    return files_[level];
}

VersionBuilder::VersionBuilder(const InternalKeyComparator& comparator, const Version& base)
    : comparator_(&comparator) {
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        for (const Version::File& file : base.files(level)) {
            levels_[level].emplace(file->number, file);
        }
    }
}

Status VersionBuilder::Apply(const VersionEdit& edit) {
    for (const DeletedFile& deleted : edit.deleted_files()) {
        if (levels_[deleted.level].erase(deleted.number) == 0) {
            return std::unexpected(Error::InvalidArgument(
                "edit deletes file " + std::to_string(deleted.number) +
                ", which is not live in level " + std::to_string(deleted.level)));
        }
    }
    for (const NewFile& added : edit.new_files()) {
        if (IsLive(added.file.number)) {
            return std::unexpected(Error::InvalidArgument(
                "edit adds file " + std::to_string(added.file.number) + ", which is already live"));
        }
        if (comparator_->Compare(added.file.smallest, added.file.largest) > 0) {
            return std::unexpected(
                Error::InvalidArgument("file " + std::to_string(added.file.number) +
                                       " has its smallest key after its largest"));
        }
        Version::File file = MakeVersionFile(added.file);
        levels_[added.level].emplace(added.file.number, std::move(file));
    }
    return {};
}

Result<Version> VersionBuilder::Build() const {
    Version version;
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        std::vector<Version::File>& files = version.files_[level];
        files.reserve(levels_[level].size());
        for (const auto& entry : levels_[level]) {
            files.push_back(entry.second);
        }
        std::ranges::sort(files, [this](const Version::File& left, const Version::File& right) {
            const int order = comparator_->Compare(left->smallest, right->smallest);
            return order != 0 ? order < 0 : left->number < right->number;
        });
        for (std::size_t index = 1; level > 0 && index < files.size(); ++index) {
            if (comparator_->Compare(files[index - 1]->largest, files[index]->smallest) >= 0) {
                return std::unexpected(
                    Error::InvalidArgument("files " + std::to_string(files[index - 1]->number) +
                                           " and " + std::to_string(files[index]->number) +
                                           " overlap in level " + std::to_string(level)));
            }
        }
    }
    return version;
}

Version VersionBuilder::BuildTrusted(const InternalKeyComparator& comparator, const Version& base,
                                     const VersionEdit& edit) {
    PreparedChanges changes = PrepareChanges(edit);
    Version version;
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        std::vector<Version::File>& added = changes.additions[level];
        std::ranges::sort(added, [&](const Version::File& left, const Version::File& right) {
            return FileBefore(comparator, left, right);
        });
        const std::span<const Version::File> existing = base.files(level);
        std::vector<Version::File>& files = version.files_[level];
        files.reserve(existing.size() + added.size());

        const auto deleted = [&](std::uint64_t number) {
            return std::ranges::binary_search(changes.deletions[level], number);
        };
        const auto append = [&](const Version::File& file, bool existing_file) {
            if (existing_file && deleted(file->number)) {
                return;
            }
            files.push_back(file);
        };

        auto current = existing.begin();
        for (const Version::File& file : added) {
            while (current != existing.end() && FileBefore(comparator, *current, file)) {
                append(*current, true);
                ++current;
            }
            append(file, false);
        }
        while (current != existing.end()) {
            append(*current, true);
            ++current;
        }
    }
    return version;
}

bool VersionBuilder::IsLive(std::uint64_t number) const {
    return std::ranges::any_of(levels_,
                               [number](const auto& level) { return level.contains(number); });
}

}  // namespace modern_leveldb
