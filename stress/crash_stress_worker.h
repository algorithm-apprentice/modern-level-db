#ifndef MODERN_LEVELDB_STRESS_CRASH_STRESS_WORKER_H_
#define MODERN_LEVELDB_STRESS_CRASH_STRESS_WORKER_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "modern_leveldb/db.h"
#include "modern_leveldb/options.h"

namespace modern_leveldb::stress {

inline constexpr std::size_t CrashStressValueSize = 80U * 1024U;
inline constexpr std::string_view CrashStressLatestKey = "crash/latest";

enum class CrashStressWorkerMode : std::uint8_t {
    WriteHold,
    InflightHold,
    VerifyPrefix,
    VerifyOptional,
    Initialize,
    Identity,
};

enum class OptionalCrashBatchState : std::uint8_t {
    Absent,
    Complete,
};

struct CrashStressWorkerArguments {
    CrashStressWorkerMode mode = CrashStressWorkerMode::Identity;
    std::filesystem::path database;
    std::filesystem::path journal;
    std::uint64_t seed = 301;
    std::size_t index = 0;
    Compression compression = Compression::None;
    bool help = false;
};

struct CrashStressJournal {
    std::size_t acknowledged = 0;
};

[[nodiscard]] CrashStressWorkerArguments ParseCrashStressWorkerArguments(
    std::span<const std::filesystem::path> arguments);
[[nodiscard]] std::string CrashStressValue(std::uint64_t seed, std::size_t index);
[[nodiscard]] std::string CrashStressMarkerKey(std::size_t index);
[[nodiscard]] std::string CrashStressAcknowledgement(std::size_t index);
[[nodiscard]] std::string CrashStressReadiness(std::size_t index);
[[nodiscard]] CrashStressJournal ParseCrashStressJournal(
    const std::filesystem::path& path);
void InitializeCrashStressDatabase(const std::filesystem::path& database,
                                   Compression compression);
[[nodiscard]] Database OpenCrashStressDatabase(
    const std::filesystem::path& database, Compression compression, bool create);
void ApplyCrashStressBatch(Database& database, std::uint64_t seed, std::size_t index);
void WriteCrashStressBatch(const std::filesystem::path& database, std::uint64_t seed,
                           std::size_t index, Compression compression);
[[nodiscard]] std::size_t VerifyCrashStressPrefix(
    const std::filesystem::path& database, const std::filesystem::path& journal,
    std::uint64_t seed, Compression compression);
[[nodiscard]] OptionalCrashBatchState VerifyOptionalCrashStressBatch(
    const std::filesystem::path& database, std::uint64_t seed, std::size_t index,
    Compression compression);
[[nodiscard]] std::string CrashStressWorkerUsage();

}  // namespace modern_leveldb::stress

#endif  // MODERN_LEVELDB_STRESS_CRASH_STRESS_WORKER_H_
