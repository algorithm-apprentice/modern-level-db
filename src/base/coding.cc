#include "modern_leveldb/base/coding.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

template <typename UInt, std::size_t Extent>
void EncodeFixed(std::span<std::byte, Extent> output, UInt value) noexcept {
  static_assert(Extent == sizeof(UInt));
  for (std::size_t index = 0; index < Extent; ++index) {
    output[index] =
        static_cast<std::byte>((value >> (index * 8U)) & static_cast<UInt>(0xffU));
  }
}

template <typename UInt, std::size_t Extent>
UInt DecodeFixed(std::span<const std::byte, Extent> input) noexcept {
  static_assert(Extent == sizeof(UInt));
  UInt value = 0;
  for (std::size_t index = 0; index < Extent; ++index) {
    value |= std::to_integer<UInt>(input[index]) << (index * 8U);
  }
  return value;
}

template <typename UInt>
[[nodiscard]] Result<UInt> ConsumeVarint(ByteView& input) {
  static_assert(std::is_unsigned_v<UInt>);

  constexpr std::size_t PayloadBits = 7;
  constexpr std::size_t ValueBits = std::numeric_limits<UInt>::digits;
  constexpr std::size_t MaxBytes = (ValueBits + PayloadBits - 1U) / PayloadBits;

  UInt value = 0;
  for (std::size_t index = 0; index < MaxBytes; ++index) {
    if (index >= input.size()) {
      return std::unexpected(Error::Corruption("truncated varint"));
    }

    const auto byte = std::to_integer<unsigned int>(input[index]);
    const auto payload = byte & 0x7fU;
    const std::size_t shift = index * PayloadBits;

    // Match LevelDB: unsigned shifting discards excess terminal payload bits.
    value |= static_cast<UInt>(payload) << shift;
    if ((byte & 0x80U) == 0U) {
      input = input.subspan(index + 1U);
      return value;
    }

    if (index + 1U == MaxBytes) {
      return std::unexpected(Error::Corruption("varint overflow"));
    }
  }

  return std::unexpected(Error::Corruption("invalid varint"));
}

}  // namespace

void EncodeFixed32(std::span<std::byte, sizeof(std::uint32_t)> output,
                   std::uint32_t value) noexcept {
  EncodeFixed(output, value);
}

void EncodeFixed64(std::span<std::byte, sizeof(std::uint64_t)> output,
                   std::uint64_t value) noexcept {
  EncodeFixed(output, value);
}

std::uint32_t DecodeFixed32(
    std::span<const std::byte, sizeof(std::uint32_t)> input) noexcept {
  return DecodeFixed<std::uint32_t>(input);
}

std::uint64_t DecodeFixed64(
    std::span<const std::byte, sizeof(std::uint64_t)> input) noexcept {
  return DecodeFixed<std::uint64_t>(input);
}

void AppendFixed32(std::vector<std::byte>& output, std::uint32_t value) {
  std::array<std::byte, sizeof(value)> encoded;
  EncodeFixed32(encoded, value);
  output.insert(output.end(), encoded.begin(), encoded.end());
}

void AppendFixed64(std::vector<std::byte>& output, std::uint64_t value) {
  std::array<std::byte, sizeof(value)> encoded;
  EncodeFixed64(encoded, value);
  output.insert(output.end(), encoded.begin(), encoded.end());
}

Result<std::uint32_t> ConsumeFixed32(ByteView& input) {
  constexpr std::size_t EncodedSize = sizeof(std::uint32_t);
  if (input.size() < EncodedSize) {
    return std::unexpected(Error::Corruption("truncated fixed32"));
  }

  const std::uint32_t value = DecodeFixed32(input.first<EncodedSize>());
  input = input.subspan(EncodedSize);
  return value;
}

Result<std::uint64_t> ConsumeFixed64(ByteView& input) {
  constexpr std::size_t EncodedSize = sizeof(std::uint64_t);
  if (input.size() < EncodedSize) {
    return std::unexpected(Error::Corruption("truncated fixed64"));
  }

  const std::uint64_t value = DecodeFixed64(input.first<EncodedSize>());
  input = input.subspan(EncodedSize);
  return value;
}

void AppendVarint32(std::vector<std::byte>& output, std::uint32_t value) {
  while (value >= 0x80U) {
    output.push_back(static_cast<std::byte>((value & 0x7fU) | 0x80U));
    value >>= 7U;
  }
  output.push_back(static_cast<std::byte>(value));
}

void AppendVarint64(std::vector<std::byte>& output, std::uint64_t value) {
  while (value >= 0x80U) {
    output.push_back(static_cast<std::byte>((value & 0x7fU) | 0x80U));
    value >>= 7U;
  }
  output.push_back(static_cast<std::byte>(value));
}

Result<std::uint32_t> ConsumeVarint32(ByteView& input) {
  return ConsumeVarint<std::uint32_t>(input);
}

Result<std::uint64_t> ConsumeVarint64(ByteView& input) {
  return ConsumeVarint<std::uint64_t>(input);
}

Status AppendLengthPrefixed(std::vector<std::byte>& output, ByteView value) {
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return std::unexpected(Error::InvalidArgument("length-prefixed value exceeds uint32"));
  }

  std::vector<std::byte> stable_value;
  const auto before = std::less<const std::byte*>{};
  if (!value.empty() && !output.empty() && before(value.data(), output.data() + output.size()) &&
      before(output.data(), value.data() + value.size())) {
    stable_value.assign(value.begin(), value.end());
    value = stable_value;
  }

  AppendVarint32(output, static_cast<std::uint32_t>(value.size()));
  if (!value.empty()) {
    output.insert(output.end(), value.begin(), value.end());
  }
  return {};
}

Result<ByteView> ConsumeLengthPrefixed(ByteView& input) {
  ByteView remaining = input;
  Result<std::uint32_t> length = ConsumeVarint32(remaining);
  if (!length.has_value()) {
    return std::unexpected(length.error());
  }
  if (remaining.size() < *length) {
    return std::unexpected(Error::Corruption("truncated length-prefixed value"));
  }

  const ByteView value = remaining.first(*length);
  input = remaining.subspan(*length);
  return value;
}

std::size_t VarintLength(std::uint64_t value) noexcept {
  std::size_t length = 1;
  while (value >= 0x80U) {
    value >>= 7U;
    ++length;
  }
  return length;
}

}  // namespace modern_leveldb
