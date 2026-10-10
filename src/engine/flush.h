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
    // Target compaction output size, also used to cap the future overlap inherited
    // by a newly flushed table.
    std::uint64_t target_file_size = std::uint64_t{2} << 20U;
};

// Limit direct placement depth so new tables do not skip unlimited maintenance.
inline constexpr std::uint32_t MaxMemTableOutputLevel = 2;

// Places a flushed range as deep as safely possible, up to MaxMemTableOutputLevel.
// Any level-0 overlap keeps it at level 0. Otherwise stop before next-level
// overlap or grandparent bytes exceeding ten target files; disjoint placement
// preserves newest-source read order and limits the next rewrite's cost.
//
// Requires an inclusive ordered user-key range under the version's comparator,
// and a target size whose tenfold fits in 64 bits.
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
// after this one writes to, whose creation must already satisfy the selected
// backend namespace policy. Weak Windows consent is not strict name durability.
[[nodiscard]] Result<VersionEdit> FlushMemTable(
    FileSystem& file_system, const std::filesystem::path& directory,
    const InternalKeyComparator& comparator, const FlushOptions& options, TableCache& table_cache,
    const MemTable& memtable, std::uint64_t number, const Version& base, std::uint64_t log_number);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_FLUSH_H_
