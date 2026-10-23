#ifndef MODERN_LEVELDB_BASE_HASH_H_
#define MODERN_LEVELDB_BASE_HASH_H_

#include <cstdint>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

// Non-cryptographic hash for cache sharding and Bloom probes. Empty input returns
// the seed; collisions are expected and do not establish key equality.
// Borrows the stable descriptor and backing bytes only until return.
[[nodiscard]] std::uint32_t Hash32(const ByteView& input, std::uint32_t seed) noexcept;

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_HASH_H_
