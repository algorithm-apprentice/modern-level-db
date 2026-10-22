#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "table/full_filter_probe.h"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace modern_leveldb::full_filter_detail {
namespace {

constexpr std::uint32_t RequiredLeafOne = (1U << 26U) | (1U << 27U) | (1U << 28U);
constinit std::atomic<Probe> ProcessProbe{nullptr};

bool HardwareHasAvx2() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int registers[4]{};
    __cpuidex(registers, 0, 0);
    const auto maximum_leaf = static_cast<std::uint32_t>(registers[0]);
    if (maximum_leaf < 7) {
        return false;
    }
    __cpuidex(registers, 1, 0);
    const auto leaf1_ecx = static_cast<std::uint32_t>(registers[2]);
    if ((leaf1_ecx & RequiredLeafOne) != RequiredLeafOne) {
        return false;
    }
    // OSXSAVE must be present before XGETBV itself is safe to execute.
    const auto xcr0 = _xgetbv(0);
    __cpuidex(registers, 7, 0);
    return SupportsAvx2({maximum_leaf, leaf1_ecx, static_cast<std::uint32_t>(registers[1]), xcr0});
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    // The compiler runtime includes the OS XCR0 admission for AVX features.
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
#else
    return false;
#endif
}

Probe DetectRuntimeProbe() noexcept { return SelectProbe(HardwareHasAvx2()); }

}  // namespace

bool SupportsAvx2(Avx2Features features) noexcept {
    return features.maximum_leaf >= 7 &&
           (features.leaf1_ecx & RequiredLeafOne) == RequiredLeafOne &&
           (features.leaf7_ebx & (1U << 5U)) != 0 && (features.xcr0 & 6U) == 6U;
}

Probe SelectProbe(bool avx2_supported) noexcept {
#if defined(MODERN_LEVELDB_HAVE_AVX2_PROBE)
    constexpr std::array<Probe, 2> probes{&ScalarMayMatch, &avx2::MayMatch};
    return probes[static_cast<std::size_t>(avx2_supported)];
#else
    static_cast<void>(avx2_supported);
    return &ScalarMayMatch;
#endif
}

Probe CachedProbe(std::atomic<Probe>& cache, Probe (*detect)() noexcept) noexcept {
    if (const auto selected = cache.load(std::memory_order_relaxed); selected != nullptr) {
        return selected;
    }
    const auto selected = detect();
    cache.store(selected, std::memory_order_relaxed);
    return selected;
}

Probe RuntimeProbe() noexcept { return CachedProbe(ProcessProbe, &DetectRuntimeProbe); }

}  // namespace modern_leveldb::full_filter_detail
