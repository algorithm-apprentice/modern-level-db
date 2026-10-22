#ifndef MODERN_LEVELDB_TABLE_FULL_FILTER_PROBE_H_
#define MODERN_LEVELDB_TABLE_FULL_FILTER_PROBE_H_

#include <cstddef>
#include <cstdint>

namespace modern_leveldb::full_filter_detail {

using Probe = bool (*)(std::uint32_t hash, std::uint32_t probes, const std::byte* region) noexcept;

// Requires 1..30 probes and one complete, stable 64-byte region.
[[nodiscard]] bool ScalarMayMatch(std::uint32_t hash, std::uint32_t probes,
                                  const std::byte* region) noexcept;

}  // namespace modern_leveldb::full_filter_detail

#endif  // MODERN_LEVELDB_TABLE_FULL_FILTER_PROBE_H_
