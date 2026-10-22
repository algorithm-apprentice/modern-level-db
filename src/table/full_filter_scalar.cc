#include <cstddef>
#include <cstdint>

#include "table/full_filter_probe.h"

namespace modern_leveldb::full_filter_detail {

bool ScalarMayMatch(std::uint32_t hash, std::uint32_t probes, const std::byte* region) noexcept {
    for (std::uint32_t probe = 0; probe < probes; ++probe) {
        const auto bit = hash >> 23U;
        if ((std::to_integer<unsigned int>(region[bit / 8]) & (1U << (bit % 8))) == 0) {
            return false;
        }
        hash *= 0x9e3779b9U;
    }
    return true;
}

}  // namespace modern_leveldb::full_filter_detail
