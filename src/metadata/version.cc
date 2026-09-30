#include "metadata/version.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <set>
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
  const int order = comparator.CompareTrusted(left->smallest.encoded(), right->smallest.encoded());
  return order != 0 ? order < 0 : left->number < right->number;
}

}  // namespace

std::span<const Version::File> Version::files(std::uint32_t level) const noexcept {
  assert(level < NumLevels);
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
      return std::unexpected(
          Error::InvalidArgument("edit deletes file " + std::to_string(deleted.number) +
                                 ", which is not live in level " + std::to_string(deleted.level)));
    }
  }
  for (const NewFile& added : edit.new_files()) {
    if (IsLive(added.file.number)) {
      return std::unexpected(Error::InvalidArgument(
          "edit adds file " + std::to_string(added.file.number) + ", which is already live"));
    }
    if (comparator_->Compare(added.file.smallest, added.file.largest) > 0) {
      return std::unexpected(Error::InvalidArgument("file " + std::to_string(added.file.number) +
                                                    " has its smallest key after its largest"));
    }
    FileMetadata file = added.file;
    file.allowed_seeks = InitialAllowedSeeks(file.file_size);
    levels_[added.level].emplace(file.number,
                                 std::make_shared<const FileMetadata>(std::move(file)));
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
        return std::unexpected(Error::InvalidArgument(
            "files " + std::to_string(files[index - 1]->number) + " and " +
            std::to_string(files[index]->number) + " overlap in level " + std::to_string(level)));
      }
    }
  }
  return version;
}

Version VersionBuilder::BuildTrusted(const InternalKeyComparator& comparator, const Version& base,
                                     const VersionEdit& edit) {
  std::array<std::size_t, NumLevels> deletion_counts{};
  std::array<std::size_t, NumLevels> addition_counts{};
  for (const DeletedFile& deleted : edit.deleted_files()) {
    ++deletion_counts[deleted.level];
  }
  for (const NewFile& added : edit.new_files()) {
    ++addition_counts[added.level];
  }

  std::array<std::vector<std::uint64_t>, NumLevels> deletions;
  std::array<std::vector<Version::File>, NumLevels> additions;
  for (std::uint32_t level = 0; level < NumLevels; ++level) {
    deletions[level].reserve(deletion_counts[level]);
    additions[level].reserve(addition_counts[level]);
  }
  for (const DeletedFile& deleted : edit.deleted_files()) {
    deletions[deleted.level].push_back(deleted.number);
  }

#ifndef NDEBUG
  std::set<std::uint64_t> live;
  for (std::uint32_t level = 0; level < NumLevels; ++level) {
    for (const Version::File& file : base.files(level)) {
      assert(live.insert(file->number).second);
    }
    for (const std::uint64_t number : deletions[level]) {
      const auto found = std::ranges::find_if(
          base.files(level),
          [number](const Version::File& file) { return file->number == number; });
      assert(found != base.files(level).end());
      assert(live.erase(number) == 1);
    }
  }
#endif

  for (const NewFile& added : edit.new_files()) {
    assert(comparator.CompareTrusted(added.file.smallest.encoded(), added.file.largest.encoded()) <=
           0);
#ifndef NDEBUG
    assert(live.insert(added.file.number).second);
#endif
    FileMetadata file = added.file;
    file.allowed_seeks = InitialAllowedSeeks(file.file_size);
    additions[added.level].push_back(std::make_shared<const FileMetadata>(std::move(file)));
  }

  Version version;
  for (std::uint32_t level = 0; level < NumLevels; ++level) {
    std::vector<Version::File>& added = additions[level];
    std::ranges::sort(added, [&](const Version::File& left, const Version::File& right) {
      return FileBefore(comparator, left, right);
    });
    const std::span<const Version::File> existing = base.files(level);
    std::vector<Version::File>& files = version.files_[level];
    files.reserve(existing.size() + added.size());

    const auto deleted = [&](std::uint64_t number) {
      return std::ranges::binary_search(deletions[level], number);
    };
    const auto append = [&](const Version::File& file, bool existing_file) {
      if (existing_file && deleted(file->number)) {
        return;
      }
      if (level > 0 && !files.empty()) {
        assert(comparator.CompareTrusted(files.back()->largest.encoded(),
                                         file->smallest.encoded()) < 0);
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
