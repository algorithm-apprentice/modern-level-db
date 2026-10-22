#ifndef MODERN_LEVELDB_TABLE_FULL_FILTER_PROBE_H_
#define MODERN_LEVELDB_TABLE_FULL_FILTER_PROBE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace modern_leveldb::full_filter_detail {

// Every leaf requires 1..30 probes and a complete, stable 64-byte region.
using Probe = bool (*)(std::uint32_t hash, std::uint32_t probes, const std::byte* region) noexcept;

struct Avx2Features {
    std::uint32_t maximum_leaf;
    std::uint32_t leaf1_ecx;
    std::uint32_t leaf7_ebx;
    std::uint64_t xcr0;
};

[[nodiscard]] bool SupportsAvx2(Avx2Features features) noexcept;
// A true argument must represent both CPU and OS YMM-state support.
[[nodiscard]] Probe SelectProbe(bool avx2_supported) noexcept;
[[nodiscard]] Probe RuntimeProbe() noexcept;

// Cache starts null or holds this detector's selection. The concurrently safe
// detector returns a nonnull, process-stable leaf; its code needs no publication.
[[nodiscard]] Probe CachedProbe(std::atomic<Probe>& cache, Probe (*detect)() noexcept) noexcept;

// Requires 1..30 probes and one complete, stable 64-byte region.
[[nodiscard]] bool ScalarMayMatch(std::uint32_t hash, std::uint32_t probes,
                                  const std::byte* region) noexcept;

namespace avx2 {

// Defined only by the AVX2 translation unit; requires CPU and OS YMM support.
[[nodiscard]] bool MayMatch(std::uint32_t hash, std::uint32_t probes,
                            const std::byte* region) noexcept;

}  // namespace avx2

}  // namespace modern_leveldb::full_filter_detail

#endif  // MODERN_LEVELDB_TABLE_FULL_FILTER_PROBE_H_
