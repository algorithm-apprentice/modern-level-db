#include "modern_leveldb/base/crc32c.h"

#include <crc32c/crc32c.h>

#include <bit>
#include <cstddef>
#include <cstdint>

namespace modern_leveldb {
namespace {

constexpr std::uint32_t MaskDelta = 0xa282ead8U;

}  // namespace

std::uint32_t Crc32c(ByteView input) noexcept { return ExtendCrc32c(0U, input); }

std::uint32_t ExtendCrc32c(std::uint32_t crc, ByteView input) noexcept {
  if (input.empty()) {
    return crc;
  }
  return ::crc32c::Extend(crc, reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
}

std::uint32_t MaskCrc32c(std::uint32_t crc) noexcept { return std::rotr(crc, 15) + MaskDelta; }

std::uint32_t UnmaskCrc32c(std::uint32_t masked_crc) noexcept {
  return std::rotl(masked_crc - MaskDelta, 15);
}

}  // namespace modern_leveldb
