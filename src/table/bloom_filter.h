#ifndef MODERN_LEVELDB_TABLE_BLOOM_FILTER_H_
#define MODERN_LEVELDB_TABLE_BLOOM_FILTER_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

// Probabilistic exclusion for byte-equal keys: a valid filter can rule a key out,
// while "may match" still requires a table lookup. Builders/readers keep a policy copy.
class BloomFilterPolicy final {
public:
    // Density controls space/probe tradeoffs; size bounds are enforced by builders.
    explicit BloomFilterPolicy(std::uint32_t bits_per_key) noexcept;

    [[nodiscard]] std::string_view Name() const noexcept { return "leveldb.BuiltinBloomFilter2"; }

    // Returns the size of a filter over key_count keys, including its probe
    // count. A size beyond 2^61 bytes saturates.
    [[nodiscard]] std::uint64_t FilterSize(std::size_t key_count) const noexcept;

    // Appends one filter over the keys, leaving the existing bytes unchanged.
    void CreateFilter(std::span<const ByteView> keys, std::vector<std::byte>& output) const;

    // A filter shorter than two bytes matches nothing; an unknown probe encoding
    // (>30) conservatively matches every key instead of risking a false negative.
    [[nodiscard]] bool KeyMayMatch(ByteView key, ByteView filter) const noexcept;

private:
    std::uint32_t bits_per_key_;
    std::uint32_t probes_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_BLOOM_FILTER_H_
