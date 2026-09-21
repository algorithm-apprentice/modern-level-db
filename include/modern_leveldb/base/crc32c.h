#ifndef MODERN_LEVELDB_BASE_CRC32C_H_
#define MODERN_LEVELDB_BASE_CRC32C_H_

#include <cstdint>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

// Returns the unmasked Castagnoli CRC32C; empty input returns zero.
[[nodiscard]] std::uint32_t Crc32c(ByteView input) noexcept;

// Extends an unmasked, finalized checksum, not an internal CRC register.
[[nodiscard]] std::uint32_t ExtendCrc32c(std::uint32_t crc, ByteView input) noexcept;

// LevelDB's reversible transform for checksums stored in WAL and SSTable files.
[[nodiscard]] std::uint32_t MaskCrc32c(std::uint32_t crc) noexcept;
[[nodiscard]] std::uint32_t UnmaskCrc32c(std::uint32_t masked_crc) noexcept;

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_CRC32C_H_
