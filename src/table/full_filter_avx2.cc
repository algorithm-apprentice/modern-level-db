#include <immintrin.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "table/full_filter_probe.h"

namespace modern_leveldb::full_filter_detail::avx2 {
namespace {

constexpr auto Powers = [] {
    std::array<std::uint32_t, 9> values{1};
    for (std::size_t index = 1; index < values.size(); ++index) {
        values[index] = values[index - 1] * 0x9e3779b9U;
    }
    return values;
}();

}  // namespace

bool MayMatch(std::uint32_t hash, std::uint32_t probes, const std::byte* region) noexcept {
    const auto lower = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(region));
    const auto upper = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(region + 32));
    const auto multipliers = _mm256_setr_epi32(
        std::bit_cast<int>(Powers[0]), std::bit_cast<int>(Powers[1]), std::bit_cast<int>(Powers[2]),
        std::bit_cast<int>(Powers[3]), std::bit_cast<int>(Powers[4]), std::bit_cast<int>(Powers[5]),
        std::bit_cast<int>(Powers[6]), std::bit_cast<int>(Powers[7]));
    const auto advance = _mm256_set1_epi32(std::bit_cast<int>(Powers[8]));
    const auto lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const auto ones = _mm256_set1_epi32(1);
    const auto low_five = _mm256_set1_epi32(31);
    auto hashes = _mm256_mullo_epi32(_mm256_set1_epi32(std::bit_cast<int>(hash)), multipliers);
    for (std::uint32_t offset = 0; offset < probes; offset += 8) {
        const auto addresses = _mm256_srli_epi32(hashes, 28);
        const auto from_lower = _mm256_permutevar8x32_epi32(lower, addresses);
        const auto from_upper = _mm256_permutevar8x32_epi32(upper, addresses);
        const auto words =
            _mm256_blendv_epi8(from_lower, from_upper, _mm256_srai_epi32(hashes, 31));
        const auto bits = _mm256_and_si256(_mm256_srli_epi32(hashes, 23), low_five);
        const auto masks = _mm256_sllv_epi32(ones, bits);
        const auto active =
            _mm256_cmpgt_epi32(_mm256_set1_epi32(static_cast<int>(probes - offset)), lanes);
        // Unused lanes in the final group cannot require additional filter bits.
        if (_mm256_testc_si256(words, _mm256_and_si256(masks, active)) == 0) {
            return false;
        }
        hashes = _mm256_mullo_epi32(hashes, advance);
    }
    return true;
}

}  // namespace modern_leveldb::full_filter_detail::avx2
