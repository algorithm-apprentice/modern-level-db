#ifndef MODERN_LEVELDB_STRESS_STRESS_H_
#define MODERN_LEVELDB_STRESS_STRESS_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "modern_leveldb/options.h"
#include "support/database_model.h"

namespace modern_leveldb::stress {

struct StressArguments {
    std::filesystem::path database;
    std::filesystem::path output;
    std::uint64_t seed = 301;
    std::size_t operations = 100'000;
    Compression compression = Compression::None;
    bool help = false;
};

struct StressBuildIdentity {
    std::string revision;
    bool dirty = false;
};

struct StressSummary {
    std::size_t operations = 0;
    std::size_t entries = 0;

    friend bool operator==(const StressSummary&, const StressSummary&) = default;
};

class StressObserver {
public:
    StressObserver() = default;
    StressObserver(const StressObserver&) = delete;
    StressObserver& operator=(const StressObserver&) = delete;
    StressObserver(StressObserver&&) = delete;
    StressObserver& operator=(StressObserver&&) = delete;
    virtual ~StressObserver() = default;

    virtual void BeforeOperation(const test_support::ModelOperation& operation) = 0;
};

[[nodiscard]] StressArguments ParseStressArguments(
    std::span<const std::filesystem::path> arguments);
[[nodiscard]] StressSummary RunStress(const StressArguments& arguments,
                                      const StressBuildIdentity& identity,
                                      StressObserver* observer = nullptr);
[[nodiscard]] std::string StressUsage();

}  // namespace modern_leveldb::stress

#endif  // MODERN_LEVELDB_STRESS_STRESS_H_
