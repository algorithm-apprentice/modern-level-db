#include "modern_leveldb/base/coding.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t VarintPayloadBits = 7;

template <typename UInt, std::size_t Extent>
void EncodeFixed(std::span<std::byte, Extent> output, UInt value) noexcept {
    static_assert(Extent == sizeof(UInt));
    for (std::size_t index = 0; index < Extent; ++index) {
        output[index] = static_cast<std::byte>((value >> (index * 8U)) & static_cast<UInt>(0xffU));
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
void AppendFixed(std::vector<std::byte>& output, UInt value) {
    static_assert(std::is_unsigned_v<UInt>);
    std::array<std::byte, sizeof(UInt)> encoded;
    EncodeFixed(std::span(encoded), value);
    output.insert(output.end(), encoded.begin(), encoded.end());
}

template <typename UInt>
[[nodiscard]] Result<UInt> ConsumeFixed(ByteView& input, const char* truncated_message) {
    static_assert(std::is_unsigned_v<UInt>);
    constexpr std::size_t EncodedSize = sizeof(UInt);
    if (input.size() < EncodedSize) {
        return std::unexpected(Error::Corruption(truncated_message));
    }

    const UInt value = DecodeFixed<UInt>(input.first<EncodedSize>());
    input = input.subspan(EncodedSize);
    return value;
}

template <typename UInt>
void EncodeVarintTrusted(MutableByteView& output, UInt value) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    while (value >= 0x80U) {
        output.front() = static_cast<std::byte>((value & 0x7fU) | 0x80U);
        output = output.subspan(1);
        value >>= VarintPayloadBits;
    }
    output.front() = static_cast<std::byte>(value);
    output = output.subspan(1);
}

template <typename UInt>
bool EncodeVarint(MutableByteView& output, UInt value) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    if (output.size() < VarintLength(value)) {
        return false;
    }
    EncodeVarintTrusted(output, value);
    return true;
}

template <typename UInt>
void AppendVarintWithSize(std::vector<std::byte>& output, UInt value, std::size_t encoded_size) {
    static_assert(std::is_unsigned_v<UInt>);
    const std::size_t old_size = output.size();
    output.resize(old_size + encoded_size);
    MutableByteView remaining(output.data() + old_size, output.size() - old_size);
    EncodeVarintTrusted(remaining, value);
    assert(remaining.empty());
}

template <typename UInt>
void AppendVarint(std::vector<std::byte>& output, UInt value) {
    AppendVarintWithSize(output, value, VarintLength(value));
}

template <typename UInt>
[[nodiscard]] Result<UInt> ConsumeVarint(ByteView& input) {
    static_assert(std::is_unsigned_v<UInt>);

    constexpr std::size_t ValueBits = std::numeric_limits<UInt>::digits;
    constexpr std::size_t MaxBytes = (ValueBits + VarintPayloadBits - 1U) / VarintPayloadBits;

    UInt value = 0;
    for (std::size_t index = 0; index < MaxBytes; ++index) {
        if (index >= input.size()) {
            return std::unexpected(Error::Corruption("truncated varint"));
        }

        const auto byte = std::to_integer<unsigned int>(input[index]);
        const auto payload = byte & 0x7fU;
        const std::size_t shift = index * VarintPayloadBits;

        // This decoder accepts excess terminal payload bits by truncating them
        // to the unsigned result width; continuation past the maximum byte count is an error.
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

std::uint32_t DecodeFixed32(std::span<const std::byte, sizeof(std::uint32_t)> input) noexcept {
    return DecodeFixed<std::uint32_t>(input);
}

std::uint64_t DecodeFixed64(std::span<const std::byte, sizeof(std::uint64_t)> input) noexcept {
    return DecodeFixed<std::uint64_t>(input);
}

void AppendFixed32(std::vector<std::byte>& output, std::uint32_t value) {
    AppendFixed(output, value);
}

void AppendFixed64(std::vector<std::byte>& output, std::uint64_t value) {
    AppendFixed(output, value);
}

Result<std::uint32_t> ConsumeFixed32(ByteView& input) {
    return ConsumeFixed<std::uint32_t>(input, "truncated fixed32");
}

Result<std::uint64_t> ConsumeFixed64(ByteView& input) {
    return ConsumeFixed<std::uint64_t>(input, "truncated fixed64");
}

void AppendVarint32(std::vector<std::byte>& output, std::uint32_t value) {
    AppendVarint(output, value);
}

void AppendVarint64(std::vector<std::byte>& output, std::uint64_t value) {
    AppendVarint(output, value);
}

bool EncodeVarint32(MutableByteView& output, std::uint32_t value) noexcept {
    return EncodeVarint(output, value);
}

Result<std::uint32_t> ConsumeVarint32(ByteView& input) {
    return ConsumeVarint<std::uint32_t>(input);
}

Result<std::uint64_t> ConsumeVarint64(ByteView& input) {
    return ConsumeVarint<std::uint64_t>(input);
}

void AppendLengthPrefixed(std::vector<std::byte>& output, ByteView value) {
    const auto length = static_cast<std::uint32_t>(value.size());
    const std::size_t prefix_size = VarintLength(length);
    const std::size_t old_size = output.size();
    output.resize(old_size + prefix_size + value.size());

    MutableByteView prefix(output.data() + old_size, prefix_size);
    EncodeVarintTrusted(prefix, length);
    assert(prefix.empty());
    std::ranges::copy(value, output.data() + old_size + prefix_size);
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
