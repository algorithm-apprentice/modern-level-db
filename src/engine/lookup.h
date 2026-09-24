#ifndef MODERN_LEVELDB_ENGINE_LOOKUP_H_
#define MODERN_LEVELDB_ENGINE_LOOKUP_H_

#include <cstddef>
#include <optional>
#include <vector>

#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "memory/memtable.h"
#include "metadata/version.h"
#include "modern_leveldb/base/result.h"
#include "table/table.h"

namespace modern_leveldb {

// Returns the value of the newest entry of the key's user key whose sequence is
// at most the key's sequence, or nothing if that entry is a deletion or there
// is none. The memtable is searched first, then the immutable memtable if there
// is one, then the version: its level-0 files whose range holds the user key
// from newest to oldest, then at most one file in each deeper level. The first
// source with an entry decides, and later tables are not opened. The errors of
// the table cache and the tables are returned unchanged.
//
// Every source and the table cache must use the comparator. The caller captures
// the memtables and the version together and keeps them alive during the call.
[[nodiscard]] Result<std::optional<std::vector<std::byte>>> LookupValue(
    const MemTable& memtable, const MemTable* immutable, const Version& version,
    TableCache& table_cache, const InternalKeyComparator& comparator, const LookupKey& key,
    const TableReadOptions& options = {});
Result<std::optional<std::vector<std::byte>>> LookupValue(
    const MemTable& memtable, const MemTable* immutable, const Version& version,
    TableCache& table_cache, const InternalKeyComparator&& comparator, const LookupKey& key,
    const TableReadOptions& options = {}) = delete;

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_LOOKUP_H_
