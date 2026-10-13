#include "stress/mixed_stress.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace modern_leveldb::stress {
namespace {

constexpr std::uint64_t KeyMask = 0xe220a8397b1dcdafULL;

std::uint64_t Mix(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

void AppendBigEndian(std::string& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<char>(value >> shift));
    }
}

}  // namespace

MixedOperationKind SelectMixedOperation(std::uint32_t percentile) {
    if (percentile < 10) {
        return MixedOperationKind::Read;
    }
    if (percentile < 30) {
        return MixedOperationKind::Prefix;
    }
    if (percentile < 75) {
        return MixedOperationKind::Write;
    }
    if (percentile < 90) {
        return MixedOperationKind::Delete;
    }
    if (percentile < 100) {
        return MixedOperationKind::Iterate;
    }
    throw std::invalid_argument("mixed operation percentile must be below 100");
}

std::string MixedKey(std::uint64_t slot) {
    const std::uint64_t permuted = std::byteswap(slot) ^ KeyMask;
    std::string result;
    result.reserve(8);
    AppendBigEndian(result, permuted);
    return result;
}

std::string_view MixedKeyPrefix(std::string_view key) {
    if (key.size() != 8) {
        throw std::invalid_argument("mixed key must contain eight bytes");
    }
    return key.substr(0, 2);
}

std::string MixedValue(std::uint64_t seed, std::size_t worker, std::size_t operation,
                       std::uint64_t token) {
    const std::size_t length = (static_cast<std::size_t>(token % 3U) + 1U) * 8U;
    std::uint64_t state = seed ^ Mix(static_cast<std::uint64_t>(worker)) ^
                          Mix(static_cast<std::uint64_t>(operation)) ^ Mix(token);
    std::string result;
    result.reserve(length);
    while (result.size() != length) {
        state = Mix(state);
        AppendBigEndian(result, state);
    }
    return result;
}

std::vector<std::size_t> MixedEpochOperationCounts(std::size_t operations_per_thread,
                                                   std::size_t reopens) {
    if (operations_per_thread == 0) {
        throw std::invalid_argument("mixed operations per thread must be positive");
    }
    if (reopens >= operations_per_thread) {
        throw std::invalid_argument("mixed reopens must be below operations per thread");
    }
    const std::size_t epochs = reopens + 1U;
    const std::size_t base = operations_per_thread / epochs;
    const std::size_t remainder = operations_per_thread % epochs;
    std::vector<std::size_t> result(epochs, base);
    for (std::size_t epoch = 0; epoch < remainder; ++epoch) {
        ++result[epoch];
    }
    return result;
}

}  // namespace modern_leveldb::stress
