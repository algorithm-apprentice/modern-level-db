#ifndef MODERN_LEVELDB_OPTIONS_H_
#define MODERN_LEVELDB_OPTIONS_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "modern_leveldb/base/comparator.h"

namespace modern_leveldb {

class Snapshot;

enum class Compression {
  None = 0,
  Snappy = 1,
  Zstd = 2,
};

struct Options {
  // Null selects BytewiseComparator. A custom comparator is retained by every
  // database and child handle that uses it.
  std::shared_ptr<const Comparator> comparator;
  bool create_if_missing = false;
  bool error_if_exists = false;
  std::size_t write_buffer_size = std::size_t{4} << 20U;
  std::uint64_t max_file_size = std::uint64_t{2} << 20U;
  std::size_t max_open_files = 1000;
  // Uses POSIX mmap for exact-size immutable table files where supported.
  // Mapped page storage faults may terminate with SIGBUS instead of returning
  // typed Io. Set false to force copied reads and typed read errors.
  bool allow_mmap_reads = true;
  std::size_t block_size = std::size_t{4} << 10U;
  std::uint32_t block_restart_interval = 16;
  // Enabling Bloom requires comparator equality to imply byte equality;
  // otherwise equivalent keys can be reported missing. Leave unset for
  // comparators that consider different byte strings equal.
  std::optional<std::uint32_t> bloom_bits_per_key;
  // Snappy matches LevelDB's default. Incompressible blocks are stored raw.
  Compression compression = Compression::Snappy;
  // Used only by Zstd; LevelDB supports levels -5 through 22.
  int zstd_compression_level = 1;
  // Syncs the initial empty WAL and requests the backend's namespace barrier
  // before accepting writes. Windows' explicit weak-namespace mode cannot
  // persist directory entries. Set false to match LevelDB's creation policy.
  bool sync_wal_creation = true;
  // Required by the owned Windows backend, whose directory entries are not
  // durably synchronized. File Sync still flushes bytes, but OS crash/power
  // loss may lose names and acknowledged writes. POSIX behavior is unchanged.
  bool allow_weak_namespace_durability = false;
};

struct ReadOptions {
  // Must be a live snapshot from the database used by the operation.
  const Snapshot* snapshot = nullptr;
  bool fill_cache = true;
};

struct WriteOptions {
  bool sync = false;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_OPTIONS_H_
