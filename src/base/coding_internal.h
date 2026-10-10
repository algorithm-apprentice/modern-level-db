#ifndef MODERN_LEVELDB_BASE_CODING_INTERNAL_H_
#define MODERN_LEVELDB_BASE_CODING_INTERNAL_H_

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

// Requires enough output for the canonical encoding and advances past it.
void EncodeVarint32Trusted(MutableByteView& output, std::uint32_t value) noexcept;
// Requires enough output for the uint32 length and payload, then advances past both.
inline void EncodeLengthPrefixedTrusted(MutableByteView& output, ByteView value) noexcept {
    EncodeVarint32Trusted(output, static_cast<std::uint32_t>(value.size()));
    assert(output.size() >= value.size());
    std::ranges::copy(value, output.begin());
    output = output.subspan(value.size());
}

// Require a complete uint32 varint and its length-prefixed payload in stable
// backing storage. Checked decoders establish these domains once.
inline std::uint32_t ConsumeVarint32Trusted(const std::byte*& input) noexcept {
    std::uint32_t value = 0;
    std::size_t shift = 0;
    while (true) {
        const unsigned int byte = std::to_integer<unsigned int>(*input);
        ++input;
        value |= static_cast<std::uint32_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0U) {
            return value;
        }
        shift += 7U;
    }
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
