#ifndef MODERN_LEVELDB_STRESS_CONCURRENT_STRESS_H_
#define MODERN_LEVELDB_STRESS_CONCURRENT_STRESS_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "modern_leveldb/options.h"
#include "stress/stress_support.h"

namespace modern_leveldb::stress {

enum class ConcurrentStressMode : std::uint8_t {
    Checked,
    Fanout,
};

struct ConcurrentStressArguments {
    std::filesystem::path database;
    std::filesystem::path output;
    std::uint64_t seed = 301;
    ConcurrentStressMode mode = ConcurrentStressMode::Fanout;
    std::size_t threads = 32;
    std::size_t rounds = 1000;
    Compression compression = Compression::None;
    bool help = false;
};

struct ConcurrentStressSummary {
    std::size_t rounds = 0;
    std::size_t threads = 0;
    ConcurrentStressMode mode = ConcurrentStressMode::Fanout;
    std::size_t linearized_worker_operations = 0;
    std::size_t batch_writes = 0;
    std::size_t snapshot_checks = 0;
    std::size_t entries = 0;

    friend bool operator==(const ConcurrentStressSummary&,
                           const ConcurrentStressSummary&) = default;
};

class ConcurrentStressObserver {
public:
    ConcurrentStressObserver() = default;
    ConcurrentStressObserver(const ConcurrentStressObserver&) = delete;
    ConcurrentStressObserver& operator=(const ConcurrentStressObserver&) = delete;
    ConcurrentStressObserver(ConcurrentStressObserver&&) = delete;
    ConcurrentStressObserver& operator=(ConcurrentStressObserver&&) = delete;
    virtual ~ConcurrentStressObserver() = default;

    virtual void BeforeLinearizabilityRound(std::size_t round) = 0;
};

[[nodiscard]] ConcurrentStressArguments ParseConcurrentStressArguments(
    std::span<const std::filesystem::path> arguments);
[[nodiscard]] std::string ConcurrentStressModeName(ConcurrentStressMode mode);
[[nodiscard]] ConcurrentStressSummary RunConcurrentStress(
    const ConcurrentStressArguments& arguments, const StressBuildIdentity& identity,
    ConcurrentStressObserver* observer = nullptr);
[[nodiscard]] std::string ConcurrentStressUsage();

}  // namespace modern_leveldb::stress

#endif  // MODERN_LEVELDB_STRESS_CONCURRENT_STRESS_H_
