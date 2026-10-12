#include "modern_leveldb/base/hash.h"

#include <cstddef>
#include <cstdint>

#include "modern_leveldb/base/coding.h"

namespace modern_leveldb {
namespace {

constexpr std::uint32_t Multiplier = 0xc6a4a793U;

void MixHashWord(std::uint32_t& hash, std::uint32_t word) noexcept {
    hash = (hash + word) * Multiplier;
    hash ^= hash >> 16U;
}

void FoldHashWords(ByteView& input, std::uint32_t& hash) noexcept {
    while (input.size() >= sizeof(std::uint32_t)) {
        const auto word = ConsumeFixed32(input);
        MixHashWord(hash, *word);
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

    FoldHashWords(input, hash);
    FoldHashTail(input, hash);
    return hash;
}

}  // namespace modern_leveldb
