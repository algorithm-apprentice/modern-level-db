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
  std::size_t block_size = std::size_t{4} << 10U;
  std::uint32_t block_restart_interval = 16;
  std::optional<std::uint32_t> bloom_bits_per_key;
  // Snappy matches LevelDB's default. Incompressible blocks are stored raw.
  Compression compression = Compression::Snappy;
  // Used only by Zstd; LevelDB supports levels -5 through 22.
  int zstd_compression_level = 1;
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
