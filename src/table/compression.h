#ifndef MODERN_LEVELDB_TABLE_COMPRESSION_H_
#define MODERN_LEVELDB_TABLE_COMPRESSION_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

enum class BlockCompression : std::uint8_t {
  None = 0,
  Snappy = 1,
  Zstd = 2,
};

inline constexpr int MinZstdCompressionLevel = -5;
inline constexpr int MaxZstdCompressionLevel = 22;

[[nodiscard]] Status ValidateCompressionOptions(BlockCompression type, int zstd_level);

[[nodiscard]] constexpr bool CompressionIsWorthwhile(std::size_t raw_size,
                                                     std::size_t compressed_size) noexcept {
  return compressed_size < raw_size - raw_size / 8U;
}

// Reuses scratch and returns whether it contains the representation to store.
// False means the caller stores raw with type None.
[[nodiscard]] bool TryCompressBlock(ByteView raw, BlockCompression requested, int zstd_level,
                                    std::vector<std::byte>& scratch);

// Decompresses a raw Snappy block or a self-describing Zstd frame.
[[nodiscard]] Result<std::vector<std::byte>> DecompressBlock(ByteView stored,
                                                             BlockCompression type);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_COMPRESSION_H_
