#ifndef MODERN_LEVELDB_STRESS_MIXED_STRESS_H_
#define MODERN_LEVELDB_STRESS_MIXED_STRESS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace modern_leveldb::stress {

enum class MixedOperationKind : std::uint8_t {
    Read,
    Prefix,
    Write,
    Delete,
    Iterate,
};

[[nodiscard]] MixedOperationKind SelectMixedOperation(std::uint32_t percentile);
[[nodiscard]] std::string MixedKey(std::uint64_t slot);
[[nodiscard]] std::string_view MixedKeyPrefix(std::string_view key);
[[nodiscard]] std::string MixedValue(std::uint64_t seed, std::size_t worker, std::size_t operation,
                                     std::uint64_t token);
[[nodiscard]] std::vector<std::size_t> MixedEpochOperationCounts(std::size_t operations_per_thread,
                                                                 std::size_t reopens);

}  // namespace modern_leveldb::stress

#endif  // MODERN_LEVELDB_STRESS_MIXED_STRESS_H_
