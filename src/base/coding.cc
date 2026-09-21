#include "modern_leveldb/base/coding.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

template <typename UInt>
[[nodiscard]] Result<UInt> ConsumeVarint(ByteView& input) {
  static_assert(std::is_unsigned_v<UInt>);

  constexpr std::size_t kPayloadBits = 7;
  constexpr std::size_t kValueBits = std::numeric_limits<UInt>::digits;
  constexpr std::size_t kMaxBytes = (kValueBits + kPayloadBits - 1U) / kPayloadBits;

  UInt value = 0;
  for (std::size_t index = 0; index < kMaxBytes; ++index) {
    if (index >= input.size()) {
      return std::unexpected(Error::Corruption("truncated varint"));
    }

    const auto byte = std::to_integer<unsigned int>(input[index]);
    const auto payload = byte & 0x7fU;
    const std::size_t shift = index * kPayloadBits;
    const std::size_t remaining_bits = kValueBits - shift;
    const unsigned int maximum_payload =
        remaining_bits >= kPayloadBits ? 0x7fU : (1U << remaining_bits) - 1U;

    if (payload > maximum_payload) {
      return std::unexpected(Error::Corruption("varint overflow"));
    }

    value |= static_cast<UInt>(payload) << shift;
    if ((byte & 0x80U) == 0U) {
      input = input.subspan(index + 1U);
      return value;
    }

    if (index + 1U == kMaxBytes) {
      return std::unexpected(Error::Corruption("varint overflow"));
    }
  }

  return std::unexpected(Error::Corruption("invalid varint"));
}

}  // namespace

void AppendFixed32(std::vector<std::byte>& output, std::uint32_t value) {
  for (std::size_t shift = 0; shift < 32U; shift += 8U) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void AppendFixed64(std::vector<std::byte>& output, std::uint64_t value) {
  for (std::size_t shift = 0; shift < 64U; shift += 8U) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

Result<std::uint32_t> ConsumeFixed32(ByteView& input) {
  constexpr std::size_t kEncodedSize = sizeof(std::uint32_t);
  if (input.size() < kEncodedSize) {
    return std::unexpected(Error::Corruption("truncated fixed32"));
  }

  std::uint32_t value = 0;
  for (std::size_t index = 0; index < kEncodedSize; ++index) {
    value |= std::to_integer<std::uint32_t>(input[index]) << (index * 8U);
  }
  input = input.subspan(kEncodedSize);
  return value;
}

Result<std::uint64_t> ConsumeFixed64(ByteView& input) {
  constexpr std::size_t kEncodedSize = sizeof(std::uint64_t);
  if (input.size() < kEncodedSize) {
    return std::unexpected(Error::Corruption("truncated fixed64"));
  }

  std::uint64_t value = 0;
  for (std::size_t index = 0; index < kEncodedSize; ++index) {
    value |= std::to_integer<std::uint64_t>(input[index]) << (index * 8U);
  }
  input = input.subspan(kEncodedSize);
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

  AppendVarint32(output, static_cast<std::uint32_t>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
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
