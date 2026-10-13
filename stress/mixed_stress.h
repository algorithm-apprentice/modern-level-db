#ifndef MODERN_LEVELDB_STRESS_MIXED_STRESS_H_
#define MODERN_LEVELDB_STRESS_MIXED_STRESS_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "modern_leveldb/options.h"
#include "stress/stress_support.h"

namespace modern_leveldb::stress {

enum class MixedOperationKind : std::uint8_t {
    Read,
    Prefix,
    Write,
    Delete,
    Iterate,
};

[[nodiscard]] MixedOperationKind SelectMixedOperation(std::uint32_t percentile);
[[nodiscard]] std::string MixedKey(std::uint64_t slot);
[[nodiscard]] std::string_view MixedKeyPrefix(std::string_view key);
[[nodiscard]] std::uint64_t MixedRandom(std::uint64_t seed, std::size_t worker,
                                        std::size_t operation, std::uint64_t stream);
[[nodiscard]] std::string MixedValue(std::uint64_t seed, std::size_t worker, std::size_t operation,
                                     std::uint64_t token);
[[nodiscard]] std::vector<std::size_t> MixedEpochOperationCounts(std::size_t operations_per_thread,
                                                                 std::size_t reopens);

struct MixedStressArguments {
    std::filesystem::path database;
    std::filesystem::path output;
    std::uint64_t seed = 301;
    std::size_t threads = 32;
    std::size_t operations_per_thread = 1'200'000;
    std::uint64_t keys = 1'048'576;
    std::size_t reopens = 10;
    Compression compression = Compression::Snappy;
    bool help = false;
};

struct MixedStressSummary {
    std::size_t threads = 0;
    std::size_t operations_per_thread = 0;
    std::size_t reopens = 0;
    std::size_t reads = 0;
    std::size_t prefixes = 0;
    std::size_t writes = 0;
    std::size_t deletes = 0;
    std::size_t iterations = 0;
    std::size_t fresh_write_plans = 0;
    std::size_t exact_checks = 0;
    std::size_t entries = 0;
    std::string fingerprint;

    friend bool operator==(const MixedStressSummary&, const MixedStressSummary&) = default;
};

class MixedStressObserver {
public:
    MixedStressObserver() = default;
    MixedStressObserver(const MixedStressObserver&) = delete;
    MixedStressObserver& operator=(const MixedStressObserver&) = delete;
    MixedStressObserver(MixedStressObserver&&) = delete;
    MixedStressObserver& operator=(MixedStressObserver&&) = delete;
    virtual ~MixedStressObserver() = default;

    virtual void BeforeEpoch(std::size_t epoch) = 0;
};

[[nodiscard]] MixedStressArguments ParseMixedStressArguments(
    std::span<const std::filesystem::path> arguments);
[[nodiscard]] MixedStressSummary RunMixedStress(const MixedStressArguments& arguments,
                                                const StressBuildIdentity& identity,
                                                MixedStressObserver* observer = nullptr);
[[nodiscard]] std::string MixedStressUsage();

}  // namespace modern_leveldb::stress

#endif  // MODERN_LEVELDB_STRESS_MIXED_STRESS_H_
