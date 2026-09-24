#ifndef MODERN_LEVELDB_ENGINE_FLUSH_H_
#define MODERN_LEVELDB_ENGINE_FLUSH_H_

#include <cstdint>
#include <filesystem>

#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "memory/memtable.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/table_builder.h"

namespace modern_leveldb {

struct FlushOptions {
  TableBuilderOptions table_options{};
  // LevelDB's max_file_size, the size at which compaction splits its outputs.
  // A memtable's table stays above a level whose next level's files that
  // overlap it total more than ten times this size.
  std::uint64_t target_file_size = std::uint64_t{2} << 20U;
};

// The deepest level at which a memtable's table starts, LevelDB's
// kMaxMemCompactLevel.
inline constexpr std::uint32_t MaxMemTableOutputLevel = 2;

// Returns the level for a new table that holds the user keys from the smallest
// to the largest, as LevelDB's Version::PickLevelForMemTableOutput does: 0 if a
// level-0 file overlaps the range, and otherwise the first level below
// MaxMemTableOutputLevel whose next level overlaps the range or whose level
// after that has files overlapping it that total more than ten times the target
// file size, or MaxMemTableOutputLevel if there is none. A file overlaps the
// range if it holds one of its keys, both ends included. Requires the smallest
// key not after the largest under the user comparator that orders the
// version's keys, and a target file size whose tenfold fits in 64 bits.
[[nodiscard]] std::uint32_t PickLevelForMemTableOutput(const Version& version,
                                                       const Comparator& user_comparator,
                                                       ByteView smallest_user_key,
                                                       ByteView largest_user_key,
                                                       std::uint64_t target_file_size);

// Writes the memtable to table `number` with BuildTable, syncs the directory,
// and returns an edit that adds the table at the level that
// PickLevelForMemTableOutput picks in `base`, sets the log number to
// `log_number`, and sets the previous log number to zero. An empty memtable
// performs no file operation, and its edit adds no file. On failure, the table
// that BuildTable could not remove or whose directory sync failed stays for
// obsolete-file cleanup.
//
// Touches only its arguments, so the caller need not hold the database mutex.
// The number must be a fresh file number that the caller keeps from
// obsolete-file cleanup, and `log_number` must name the log that the memtable
// after this one writes to, which must already be durable in the directory.
[[nodiscard]] Result<VersionEdit> FlushMemTable(
    FileSystem& file_system, const std::filesystem::path& directory,
    const InternalKeyComparator& comparator, const FlushOptions& options, TableCache& table_cache,
    const MemTable& memtable, std::uint64_t number, const Version& base, std::uint64_t log_number);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_FLUSH_H_
