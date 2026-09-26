#include "table/compression.h"

#include <snappy.h>
#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t MaxBlockSize = std::numeric_limits<std::uint32_t>::max();

Error CorruptCompression(const char* message) { return Error::Corruption(message); }

bool PrepareOutput(std::size_t size, std::vector<std::byte>& output) {
  if (size > std::min(MaxBlockSize, output.max_size())) {
    return false;
  }
  output.resize(size);
  return true;
}

}  // namespace

Status ValidateCompressionOptions(BlockCompression type, int zstd_level) {
  if (type == BlockCompression::None || type == BlockCompression::Snappy) {
    return {};
  }
  if (type != BlockCompression::Zstd) {
    return std::unexpected(Error::InvalidArgument("compression type is invalid"));
  }
  if (zstd_level < MinZstdCompressionLevel || zstd_level > MaxZstdCompressionLevel) {
    return std::unexpected(Error::InvalidArgument("Zstd compression level is outside [-5, 22]"));
  }
  return {};
}

bool TryCompressBlock(ByteView raw, BlockCompression requested, int zstd_level,
                      std::vector<std::byte>& scratch) {
  scratch.clear();
  if (raw.empty()) {
    return false;
  }
  if (raw.size() > MaxBlockSize) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs over 4 GiB
    return false;                   // GCOVR_EXCL_LINE: needs over 4 GiB
  }

  switch (requested) {
    case BlockCompression::None:
      return false;
    case BlockCompression::Snappy: {
      const std::size_t bound = snappy::MaxCompressedLength(raw.size());
      if (!PrepareOutput(bound, scratch)) {
        return false;
      }
      std::size_t compressed_size = bound;
      snappy::RawCompress(reinterpret_cast<const char*>(raw.data()), raw.size(),
                          reinterpret_cast<char*>(scratch.data()), &compressed_size);
      scratch.resize(compressed_size);
      break;
    }
    case BlockCompression::Zstd: {
      const std::size_t bound = ZSTD_compressBound(raw.size());
      if (ZSTD_isError(bound) != 0U || !PrepareOutput(bound, scratch)) {
        scratch.clear();
        return false;
      }
      const std::size_t compressed_size =
          ZSTD_compress(scratch.data(), scratch.size(), raw.data(), raw.size(), zstd_level);
      if (ZSTD_isError(compressed_size) != 0U) {
        scratch.clear();
        return false;
      }
      scratch.resize(compressed_size);
      break;
    }
    default:
      return false;
  }

  if (!CompressionIsWorthwhile(raw.size(), scratch.size())) {
    scratch.clear();
    return false;
  }
  return true;
}

Result<std::vector<std::byte>> DecompressBlock(ByteView stored, BlockCompression type) {
  if (type == BlockCompression::Snappy) {
    std::size_t uncompressed_size = 0;
    if (!snappy::GetUncompressedLength(reinterpret_cast<const char*>(stored.data()), stored.size(),
                                       &uncompressed_size)) {
      return std::unexpected(CorruptCompression("compressed Snappy block has an invalid length"));
    }
    std::vector<std::byte> output;
    if (!PrepareOutput(uncompressed_size, output)) {
      return std::unexpected(CorruptCompression("compressed Snappy block is too large"));
    }
    char empty_output = '\0';
    char* const destination =
        output.empty() ? &empty_output : reinterpret_cast<char*>(output.data());
    if (!snappy::RawUncompress(reinterpret_cast<const char*>(stored.data()), stored.size(),
                               destination)) {
      return std::unexpected(CorruptCompression("compressed Snappy block has invalid contents"));
    }
    return output;
  }

  if (type == BlockCompression::Zstd) {
    const unsigned long long declared = ZSTD_getFrameContentSize(stored.data(), stored.size());
    std::vector<std::byte> output;
    // The two Zstd error sentinels also exceed the block-size bound.
    static_assert(ZSTD_CONTENTSIZE_ERROR > MaxBlockSize && ZSTD_CONTENTSIZE_UNKNOWN > MaxBlockSize);
    if (declared == 0 || declared > std::min(MaxBlockSize, output.max_size())) {
      return std::unexpected(CorruptCompression("compressed Zstd block has an invalid size"));
    }
    output.resize(static_cast<std::size_t>(declared));
    const std::size_t actual =
        ZSTD_decompress(output.data(), output.size(), stored.data(), stored.size());
    if (ZSTD_isError(actual) != 0U || actual != output.size()) {
      return std::unexpected(CorruptCompression("compressed Zstd block has invalid contents"));
    }
    return output;
  }

  return std::unexpected(CorruptCompression("block has an invalid compression type"));
}

}  // namespace modern_leveldb
