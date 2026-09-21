#ifndef MODERN_LEVELDB_BASE_HASH_H_
#define MODERN_LEVELDB_BASE_HASH_H_

#include <cstdint>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

// LevelDB-compatible non-cryptographic hash. Empty input returns the seed.
[[nodiscard]] std::uint32_t Hash32(ByteView input, std::uint32_t seed) noexcept;

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_HASH_H_
