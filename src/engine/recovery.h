#ifndef MODERN_LEVELDB_ENGINE_RECOVERY_H_
#define MODERN_LEVELDB_ENGINE_RECOVERY_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "metadata/version_set.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/table_builder.h"
#include "wal/wal_io.h"

namespace modern_leveldb {

struct RecoveryOptions {
  bool create_if_missing = false;
  bool error_if_exists = false;
  // Replay writes a memtable to a table once its memory usage exceeds this.
  std::size_t write_buffer_size = 4 * 1024 * 1024;
  TableBuilderOptions table_options;
};

// The state of an opened database. The lock is released last.
struct RecoveredDatabase {
  std::unique_ptr<FileLock> lock;
  std::unique_ptr<VersionSet> versions;
  std::unique_ptr<WalWriter> log;
  std::uint64_t log_number;
};

// Locks the database directory, creates or recovers its version set, replays
// the logs that the MANIFEST does not cover into level-0 tables, and records a
// new, empty log. Damaged log records are skipped. A missing database without
// create_if_missing, or an existing one with error_if_exists, is
// InvalidArgument; missing tables and log records that no writer produces are
// Corruption. On failure the lock is released and no table the recovery wrote
// stays in the table cache.
//
// The file system, the comparator, and the table cache must outlive the
// result, and the table cache must read the directory through the same file
// system with the same comparator.
[[nodiscard]] Result<RecoveredDatabase> RecoverDatabase(FileSystem& file_system,
                                                        const std::filesystem::path& directory,
                                                        const InternalKeyComparator& comparator,
                                                        const RecoveryOptions& options,
                                                        TableCache& table_cache);
Result<RecoveredDatabase> RecoverDatabase(FileSystem& file_system,
                                          const std::filesystem::path& directory,
                                          const InternalKeyComparator&& comparator,
                                          const RecoveryOptions& options,
                                          TableCache& table_cache) = delete;

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_RECOVERY_H_
