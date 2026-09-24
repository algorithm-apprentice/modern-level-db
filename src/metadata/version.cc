#include "metadata/version.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace modern_leveldb {

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
    levels_[added.level].emplace(added.file.number,
                                 std::make_shared<const FileMetadata>(added.file));
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

bool VersionBuilder::IsLive(std::uint64_t number) const {
  return std::ranges::any_of(levels_,
                             [number](const auto& level) { return level.contains(number); });
}

}  // namespace modern_leveldb
