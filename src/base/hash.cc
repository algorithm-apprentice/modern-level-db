#include "modern_leveldb/base/hash.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace modern_leveldb {
namespace {

constexpr std::uint32_t Multiplier = 0xc6a4a793U;

void MixHashWord(std::uint32_t& hash, std::uint32_t word) noexcept {
    hash = (hash + word) * Multiplier;
    hash ^= hash >> 16U;
}

// Callers establish that a complete word remains before loading.
template <typename UInt>
UInt LoadHashWord(const std::byte* input) noexcept {
    if constexpr (std::endian::native == std::endian::little ||
                  std::endian::native == std::endian::big) {
        UInt value;
        std::memcpy(&value, input, sizeof(value));
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        return value;
    } else {
        UInt value = 0;
        for (std::size_t index = 0; index < sizeof(UInt); ++index) {
            value |= std::to_integer<UInt>(input[index]) << (index * 8U);
        }
        return value;
    }
}

void DecodeHashTail(ByteView input, std::uint32_t& tail) noexcept {
    for (std::size_t index = 0; index < input.size(); ++index) {
        tail |= std::to_integer<std::uint32_t>(input[index]) << (index * 8U);
    }
}

void MixHashTail(std::uint32_t& hash, std::uint32_t tail) noexcept {
    hash = (hash + tail) * Multiplier;
    hash ^= hash >> 24U;
}

void FoldHashTail(ByteView input, std::uint32_t& hash) noexcept {
    if (!input.empty()) {
        std::uint32_t tail = 0;
        DecodeHashTail(input, tail);
        MixHashTail(hash, tail);
    }
}

}  // namespace

std::uint32_t Hash32(ByteView input, std::uint32_t seed) noexcept {
    std::uint32_t hash = seed ^ (static_cast<std::uint32_t>(input.size()) * Multiplier);
    const std::byte* cursor = input.data();
    std::size_t remaining = input.size();
    while (remaining >= sizeof(std::uint64_t)) {
        const auto words = LoadHashWord<std::uint64_t>(cursor);
        MixHashWord(hash, static_cast<std::uint32_t>(words));
        MixHashWord(hash, static_cast<std::uint32_t>(words >> 32U));
        cursor += sizeof(std::uint64_t);
        remaining -= sizeof(std::uint64_t);
    }
    if (remaining >= sizeof(std::uint32_t)) {
        MixHashWord(hash, LoadHashWord<std::uint32_t>(cursor));
        cursor += sizeof(std::uint32_t);
        remaining -= sizeof(std::uint32_t);
    }
    FoldHashTail(ByteView(cursor, remaining), hash);
    return hash;
}

}  // namespace modern_leveldb
