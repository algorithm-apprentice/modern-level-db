#ifndef MODERN_LEVELDB_ENGINE_TABLE_CACHE_H_
#define MODERN_LEVELDB_ENGINE_TABLE_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "cache/sharded_lru_cache.h"
#include "format/internal_key.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/table.h"

namespace modern_leveldb {

// Keeps the tables of a database directory open for reuse, by file number.
// The file system, the comparator, and the options' block cache must outlive
// the table cache, and every handle must be released before it is destroyed.
// Find and Evict are safe for concurrent calls.
class TableCache final {
 public:
  // Keeps a table open.
  using Handle = ShardedLruCache<Table>::Handle;

  // Each of the cache's 16 shards has a capacity of ceil(capacity / 16)
  // tables, which it enforces only when it caches a table, by closing its
  // least recently used tables that no handle holds. The capacity is
  // therefore a target, not a limit on open files.
  TableCache(FileSystem& file_system, std::filesystem::path directory,
             const InternalKeyComparator& comparator, const TableOptions& options,
             std::size_t capacity);
  TableCache(FileSystem& file_system, std::filesystem::path directory,
             const InternalKeyComparator&& comparator, const TableOptions& options,
             std::size_t capacity) = delete;

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;
  TableCache(TableCache&&) = delete;
  TableCache& operator=(TableCache&&) = delete;
  ~TableCache() = default;

  // Returns the table of the nonzero file number, opening the table file of
  // that number with the given size unless the table is cached; a cached table
  // ignores the size. Errors are returned unchanged and are not cached.
  [[nodiscard]] Result<Handle> Find(std::uint64_t file_number, std::uint64_t file_size);

  // Stops caching the table of the file number. Handles stay valid, and a Find
  // that is opening the file may cache it again, so before deleting a table
  // file the caller must ensure that no Find for it can run.
  void Evict(std::uint64_t file_number);

 private:
  FileSystem* file_system_;
  std::filesystem::path directory_;
  const InternalKeyComparator* comparator_;
  TableOptions options_;
  ShardedLruCache<Table> tables_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_TABLE_CACHE_H_
