#ifndef MODERN_LEVELDB_DATABASE_STATE_H_
#define MODERN_LEVELDB_DATABASE_STATE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

// Modern LevelDB's fixed LSM level count.
inline constexpr std::size_t DatabaseLevelCount = 7;

struct DatabaseLevelState {
  // Files in the current Version and the sum of their recorded SSTable sizes.
  std::size_t file_count = 0;
  std::uint64_t file_bytes = 0;
};

// An owning point-in-time copy of database topology and maintenance state.
struct DatabaseState {
  std::array<DatabaseLevelState, DatabaseLevelCount> levels{};

  // The sequence published to latest reads.
  std::uint64_t last_sequence = 0;
  // Explicit snapshot registrations retained by Snapshot handles or iterators
  // created from them. Duplicate registrations are counted separately.
  std::size_t snapshot_count = 0;
  std::optional<std::uint64_t> oldest_snapshot_sequence;

  // Includes the active queue leader. Memtable bytes are arena reservations,
  // not exact payload bytes or total process memory.
  std::size_t write_queue_depth = 0;
  std::size_t mutable_memtable_bytes = 0;
  std::optional<std::size_t> immutable_memtable_bytes;

  // Fresh table numbers protected from obsolete-file cleanup. The count can
  // remain nonzero after an exceptional background termination.
  std::size_t protected_output_count = 0;
  // True while an accepted engine background task is queued or running.
  bool background_work_scheduled = false;
  // The first sticky error recorded by the engine. This is not a history of
  // every exception that may have propagated to a caller.
  std::optional<Error> sticky_error;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_DATABASE_STATE_H_
