#ifndef MODERN_LEVELDB_ENGINE_LOOKUP_H_
#define MODERN_LEVELDB_ENGINE_LOOKUP_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "memory/memtable.h"
#include "metadata/version.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "table/table.h"

namespace modern_leveldb {

// A file that a point read searched without deciding the read before it
// searched another, which LevelDB charges a seek.
struct SeekCharge {
  std::uint32_t level;
  Version::File file;
};

struct PointRead {
  // The value, or nothing if the newest visible entry is a deletion or there
  // is none.
  std::optional<std::vector<std::byte>> value;
  // The first file that the read searched, if it searched another after it.
  std::optional<SeekCharge> seek;
};

// Reads the value of the newest entry of the key's user key whose sequence is
// at most the key's sequence, and nothing if that entry is a deletion or there
// is none. The memtable is searched first, then the immutable memtable if there
// is one, then the version: its level-0 files whose range holds the user key
// from newest to oldest, then at most one file in each deeper level. The first
// source with an entry decides, and later tables are not opened. A read that
// searches more than one file of the version reports the first as its seek.
// The errors of the table cache and the tables are returned unchanged.
//
// Every source and the table cache must use the comparator. The caller captures
// the memtables and the version together and keeps them alive during the call.
[[nodiscard]] Result<PointRead> LookupValue(const MemTable& memtable, const MemTable* immutable,
                                            const Version& version, TableCache& table_cache,
                                            const InternalKeyComparator& comparator,
                                            const LookupKey& key,
                                            const TableReadOptions& options = {});
Result<PointRead> LookupValue(const MemTable& memtable, const MemTable* immutable,
                              const Version& version, TableCache& table_cache,
                              const InternalKeyComparator&& comparator, const LookupKey& key,
                              const TableReadOptions& options = {}) = delete;

// Returns the first file, with its level, that a point read at the internal
// key would search in the version, if it would search at least two, as
// LevelDB's Version::RecordReadSample charges a sampled read. Requires a valid
// internal key.
[[nodiscard]] std::optional<SeekCharge> SampleCharge(const Version& version,
                                                     const InternalKeyComparator& comparator,
                                                     ByteView internal_key);
std::optional<SeekCharge> SampleCharge(const Version& version,
                                       const InternalKeyComparator&& comparator,
                                       ByteView internal_key) = delete;

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_LOOKUP_H_
