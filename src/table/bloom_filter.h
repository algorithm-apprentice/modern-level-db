#ifndef MODERN_LEVELDB_TABLE_BLOOM_FILTER_H_
#define MODERN_LEVELDB_TABLE_BLOOM_FILTER_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

// LevelDB's built-in Bloom filter policy. It is a small value, so filter
// builders and readers keep their own copy.
class BloomFilterPolicy final {
 public:
  // Any value follows LevelDB's formulas; the public API validates user options.
  explicit BloomFilterPolicy(std::uint32_t bits_per_key) noexcept;

  [[nodiscard]] std::string_view Name() const noexcept { return "leveldb.BuiltinBloomFilter2"; }

  // Returns the size of a filter over key_count keys, including its probe
  // count. A size beyond 2^61 bytes saturates.
  [[nodiscard]] std::uint64_t FilterSize(std::size_t key_count) const noexcept;

  // Appends one filter over the keys, leaving the existing bytes unchanged.
  void CreateFilter(std::span<const ByteView> keys, std::vector<std::byte>& output) const;

  // Matches any byte string as LevelDB does: a filter shorter than two bytes
  // matches nothing, and a probe count above 30 matches every key.
  [[nodiscard]] bool KeyMayMatch(ByteView key, ByteView filter) const noexcept;

 private:
  std::uint32_t bits_per_key_;
  std::uint32_t probes_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_BLOOM_FILTER_H_
