#include "modern_leveldb/base/crc32c.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace modern_leveldb {
namespace {

constexpr std::uint32_t Polynomial = 0x82f63b78U;
constexpr std::uint32_t MaskDelta = 0xa282ead8U;

constexpr std::array<std::uint32_t, 256> BuildCrc32cTable() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::size_t index = 0; index < table.size(); ++index) {
    auto value = static_cast<std::uint32_t>(index);
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1U) != 0U ? (value >> 1U) ^ Polynomial : value >> 1U;
    }
    table[index] = value;
  }
  return table;
}

constexpr auto Crc32cTable = BuildCrc32cTable();

}  // namespace

std::uint32_t Crc32c(ByteView input) noexcept { return ExtendCrc32c(0U, input); }

std::uint32_t ExtendCrc32c(std::uint32_t crc, ByteView input) noexcept {
  std::uint32_t state = ~crc;
  for (const std::byte byte : input) {
    const auto index = (state ^ std::to_integer<std::uint32_t>(byte)) & 0xffU;
    state = (state >> 8U) ^ Crc32cTable[index];
  }
  return ~state;
}

std::uint32_t MaskCrc32c(std::uint32_t crc) noexcept { return std::rotr(crc, 15) + MaskDelta; }

std::uint32_t UnmaskCrc32c(std::uint32_t masked_crc) noexcept {
  return std::rotl(masked_crc - MaskDelta, 15);
}

}  // namespace modern_leveldb
