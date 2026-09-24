#include "engine/flush.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

#include "engine/build_table.h"

namespace modern_leveldb {
namespace {

// LevelDB's MaxGrandParentOverlapBytes, relative to the target file size.
constexpr std::uint64_t GrandparentOverlapFactor = 10;

// LevelDB checks that the level after the next one exists; for every level
// that a memtable's table moves from, it does.
static_assert(MaxMemTableOutputLevel + 1 < NumLevels);

// A range of user keys, both ends included.
struct UserKeyRange {
  const Comparator* comparator;
  ByteView smallest;
  ByteView largest;

  [[nodiscard]] bool Overlaps(const FileMetadata& file) const noexcept {
    return comparator->Compare(smallest, file.largest.user_key()) <= 0 &&
           comparator->Compare(largest, file.smallest.user_key()) >= 0;
  }
};

bool OverlapInLevel(const Version& version, std::uint32_t level, const UserKeyRange& range) {
  const std::span<const Version::File> files = version.files(level);
  if (level == 0) {
    return std::ranges::any_of(files,
                               [&](const Version::File& file) { return range.Overlaps(*file); });
  }
  // Deeper levels are sorted and disjoint, so only the first file that does not
  // end before the range can overlap it.
  const auto found = std::ranges::partition_point(files, [&](const Version::File& file) {
    return range.comparator->Compare(file->largest.user_key(), range.smallest) < 0;
  });
  return found != files.end() && range.Overlaps(**found);
}

std::uint64_t OverlappingBytes(const Version& version, std::uint32_t level,
                               const UserKeyRange& range) {
  std::uint64_t total = 0;
  for (const Version::File& file : version.files(level)) {
    if (range.Overlaps(*file)) {
      total += file->file_size;
    }
  }
  return total;
}

}  // namespace

std::uint32_t PickLevelForMemTableOutput(const Version& version, const Comparator& user_comparator,
                                         ByteView smallest_user_key, ByteView largest_user_key,
                                         std::uint64_t target_file_size) {
  const UserKeyRange range{
      .comparator = &user_comparator, .smallest = smallest_user_key, .largest = largest_user_key};
  if (OverlapInLevel(version, 0, range)) {
    return 0;
  }
  const std::uint64_t grandparent_limit = GrandparentOverlapFactor * target_file_size;
  std::uint32_t level = 0;
  while (level < MaxMemTableOutputLevel) {
    if (OverlapInLevel(version, level + 1, range) ||
        OverlappingBytes(version, level + 2, range) > grandparent_limit) {
      break;
    }
    ++level;
  }
  return level;
}

Result<VersionEdit> FlushMemTable(FileSystem& file_system, const std::filesystem::path& directory,
                                  const InternalKeyComparator& comparator,
                                  const FlushOptions& options, TableCache& table_cache,
                                  const MemTable& memtable, std::uint64_t number,
                                  const Version& base, std::uint64_t log_number) {
  Result<std::optional<FileMetadata>> table = BuildTable(
      file_system, directory, comparator, options.table_options, table_cache, memtable, number);
  if (!table.has_value()) {
    return std::unexpected(std::move(table).error());
  }
  VersionEdit edit;
  edit.SetLogNumber(log_number);
  edit.SetPrevLogNumber(0);
  if (!table->has_value()) {
    return edit;
  }
  const Status synced = file_system.SyncDirectory(directory);
  if (!synced.has_value()) {
    return std::unexpected(synced.error());
  }
  FileMetadata file = std::move(*table).value();
  const std::uint32_t level =
      PickLevelForMemTableOutput(base, comparator.user_comparator(), file.smallest.user_key(),
                                 file.largest.user_key(), options.target_file_size);
  // A built table has a fresh number and valid keys at a valid level.
  const Status added = edit.AddFile(level, std::move(file));
  assert(added.has_value());
  static_cast<void>(added);
  return edit;
}

}  // namespace modern_leveldb
