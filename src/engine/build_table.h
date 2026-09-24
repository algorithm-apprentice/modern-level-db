#ifndef MODERN_LEVELDB_ENGINE_BUILD_TABLE_H_
#define MODERN_LEVELDB_ENGINE_BUILD_TABLE_H_

#include <cstdint>
#include <filesystem>
#include <optional>

#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "memory/memtable.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/table_builder.h"

namespace modern_leveldb {

// Writes the memtable's entries to table file `number` in the directory and
// finds the table in the table cache, which opens and caches it. Returns the
// table's metadata, or nothing, touching no file, for an empty memtable. After
// the file is opened, a failure removes it, ignoring an error from the removal.
//
// The table cache must read the directory through the same file system with
// the same comparator, which must order the memtable's keys. The memtable must
// not change during the build, and the number must be a fresh file number. The
// caller syncs the directory before a MANIFEST edit references the table.
[[nodiscard]] Result<std::optional<FileMetadata>> BuildTable(
    FileSystem& file_system, const std::filesystem::path& directory,
    const InternalKeyComparator& comparator, const TableBuilderOptions& options,
    TableCache& table_cache, const MemTable& memtable, std::uint64_t number);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_BUILD_TABLE_H_
