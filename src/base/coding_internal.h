#ifndef MODERN_LEVELDB_BASE_CODING_INTERNAL_H_
#define MODERN_LEVELDB_BASE_CODING_INTERNAL_H_

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

// Requires a complete unsigned word in stable backing storage; alignment is unrestricted.
template <typename UInt, std::size_t Extent>
inline UInt DecodeFixedTrusted(std::span<const std::byte, Extent> input) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    static_assert(Extent == sizeof(UInt));
    if constexpr (std::endian::native == std::endian::little) {
        UInt value;
        std::memcpy(&value, input.data(), sizeof(value));
        return value;
    } else {
        UInt value = 0;
        for (std::size_t index = 0; index < Extent; ++index) {
            value |= std::to_integer<UInt>(input[index]) << (index * 8U);
        }
        return value;
    }
}

template <typename UInt, std::size_t Extent>
inline void EncodeFixedTrusted(std::span<std::byte, Extent> output, UInt value) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    static_assert(Extent == sizeof(UInt));
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(output.data(), &value, sizeof(value));
    } else {
        for (std::size_t index = 0; index < Extent; ++index) {
            output[index] =
                static_cast<std::byte>((value >> (index * 8U)) & static_cast<UInt>(0xffU));
        }
    }
}

// Requires a seven-bit payload and a shift within the unsigned result width.
template <typename UInt>
inline void FoldVarintPayload(UInt& value, unsigned int payload, std::size_t shift) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    value |= static_cast<UInt>(payload) << shift;
}

// Requires enough output for the canonical encoding and advances past it.
void EncodeVarint32Trusted(MutableByteView& output, std::uint32_t value) noexcept;
// Requires enough output for the uint32 length and payload, then advances past both.
inline void EncodeLengthPrefixedTrusted(MutableByteView& output, ByteView value) noexcept {
    EncodeVarint32Trusted(output, static_cast<std::uint32_t>(value.size()));
    std::ranges::copy(value, output.begin());
    output = output.subspan(value.size());
}

// Requires a complete varint terminating within the result width's maximum byte
// count, in stable backing storage. Checked decoders establish this domain once.
template <typename UInt>
inline UInt ConsumeVarintTrusted(const std::byte*& input) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    UInt value = 0;
    std::size_t shift = 0;
    while (true) {
        const unsigned int byte = std::to_integer<unsigned int>(*input);
        ++input;
        FoldVarintPayload(value, byte & 0x7fU, shift);
        if ((byte & 0x80U) == 0U) {
            return value;
        }
        shift += 7U;
    }
}

inline std::uint32_t ConsumeVarint32Trusted(const std::byte*& input) noexcept {
    return ConsumeVarintTrusted<std::uint32_t>(input);
}

inline std::uint64_t ConsumeVarint64Trusted(const std::byte*& input) noexcept {
    return ConsumeVarintTrusted<std::uint64_t>(input);
}

inline std::uint32_t ConsumeVarint32Trusted(ByteView& input) noexcept {
    const std::byte* const begin = input.data();
    const std::byte* cursor = begin;
    const std::uint32_t value = ConsumeVarint32Trusted(cursor);
    input = input.subspan(static_cast<std::size_t>(cursor - begin));
    return value;
}

inline ByteView ConsumeLengthPrefixedTrusted(const std::byte*& input) noexcept {
    const std::uint32_t length = ConsumeVarint32Trusted(input);
    const ByteView value(input, length);
    input += length;
    return value;
}

inline ByteView ConsumeLengthPrefixedTrusted(ByteView& input) noexcept {
    const std::uint32_t length = ConsumeVarint32Trusted(input);
    const ByteView value = input.first(length);
    input = input.subspan(length);
    return value;
}

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_CODING_INTERNAL_H_
