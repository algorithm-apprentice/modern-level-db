#ifndef MODERN_LEVELDB_STRESS_OPTION_FAULT_STRESS_H_
#define MODERN_LEVELDB_STRESS_OPTION_FAULT_STRESS_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "modern_leveldb/base/result.h"
#include "modern_leveldb/options.h"
#include "stress/stress_support.h"
#include "support/database_model.h"

namespace modern_leveldb::stress {

struct OptionMatrixCell {
    Compression compression = Compression::None;
    bool allow_mmap_reads = true;
    std::uint32_t bloom_bits_per_key = 0;
    std::size_t write_buffer_size = 64U * 1024U;
    bool sync_wal_creation = true;

    friend bool operator==(const OptionMatrixCell&, const OptionMatrixCell&) = default;
};

struct OptionMatrixCellSummary {
    std::string id;
    std::uint64_t seed = 0;
    std::size_t operations = 0;
    std::size_t entries = 0;
    std::string fingerprint;

    friend bool operator==(const OptionMatrixCellSummary&,
                           const OptionMatrixCellSummary&) = default;
};

enum class FaultScenario : std::uint8_t {
    Read,
    WalAppend,
    WalSync,
    OldWalClose,
    TableOpen,
    TableSync,
    TableClose,
    TableDirectorySync,
    ManifestAppend,
    ManifestSync,
    Scheduling,
};

enum class RecoveredBatchState : std::uint8_t {
    Old,
    New,
};

struct FaultScenarioSummary {
    FaultScenario scenario = FaultScenario::Read;
    std::string name;
    std::string injected_operation;
    ErrorCode error_code = ErrorCode::Io;
    std::string error_message;
    bool sticky = false;
    bool later_write_reused_error_without_io = false;
    bool retry_succeeded = false;
    std::optional<RecoveredBatchState> recovered_batch;

    friend bool operator==(const FaultScenarioSummary&, const FaultScenarioSummary&) = default;
};

struct LivenessScenarioSummary {
    std::uint64_t seed = 0;
    std::size_t writes = 0;
    std::size_t background_tasks = 0;
    std::size_t table_outputs = 0;
    std::size_t point_reads = 0;
    std::size_t scans = 0;
    std::size_t entries = 0;
    std::string fingerprint;

    friend bool operator==(const LivenessScenarioSummary&,
                           const LivenessScenarioSummary&) = default;
};

struct OptionFaultStressArguments {
    std::filesystem::path workspace;
    std::uint64_t seed = 301;
    std::size_t operations = 250;
    std::size_t liveness_writes = 2'000;
    bool help = false;
};

struct OptionFaultStressSummary {
    std::size_t option_cells = 0;
    std::size_t fault_scenarios = 0;
    std::size_t operations_per_cell = 0;
    std::size_t option_entries = 0;
    LivenessScenarioSummary liveness;

    friend bool operator==(const OptionFaultStressSummary&,
                           const OptionFaultStressSummary&) = default;
};

class OptionFaultStressObserver {
public:
    OptionFaultStressObserver() = default;
    OptionFaultStressObserver(const OptionFaultStressObserver&) = delete;
    OptionFaultStressObserver& operator=(const OptionFaultStressObserver&) = delete;
    OptionFaultStressObserver(OptionFaultStressObserver&&) = delete;
    OptionFaultStressObserver& operator=(OptionFaultStressObserver&&) = delete;
    virtual ~OptionFaultStressObserver() = default;

    virtual void BeforeOptionCell(std::size_t index, const OptionMatrixCell& cell) = 0;
    virtual void BeforeFaultScenario(std::size_t index, FaultScenario scenario) = 0;
    virtual void BeforeLiveness() = 0;
};

[[nodiscard]] std::vector<OptionMatrixCell> EnumerateOptionMatrix();
[[nodiscard]] std::string OptionMatrixCellId(const OptionMatrixCell& cell);
[[nodiscard]] std::uint64_t DeriveOptionMatrixSeed(std::uint64_t campaign_seed,
                                                   const OptionMatrixCell& cell);
[[nodiscard]] std::string StableModelFingerprint(const test_support::Model& model);
[[nodiscard]] Options OptionMatrixDatabaseOptions(const OptionMatrixCell& cell);
[[nodiscard]] OptionMatrixCellSummary RunOptionMatrixCell(const std::filesystem::path& database,
                                                          const OptionMatrixCell& cell,
                                                          std::uint64_t campaign_seed,
                                                          std::size_t operations);
[[nodiscard]] std::vector<FaultScenario> EnumerateFaultScenarios();
[[nodiscard]] std::string FaultScenarioName(FaultScenario scenario);
[[nodiscard]] FaultScenarioSummary RunFaultScenario(FaultScenario scenario);
[[nodiscard]] LivenessScenarioSummary RunLivenessScenario(std::uint64_t seed, std::size_t writes);
[[nodiscard]] OptionFaultStressArguments ParseOptionFaultStressArguments(
    std::span<const std::filesystem::path> arguments);
[[nodiscard]] OptionFaultStressSummary RunOptionFaultStress(
    const OptionFaultStressArguments& arguments, const StressBuildIdentity& identity,
    OptionFaultStressObserver* observer = nullptr);
[[nodiscard]] OptionFaultStressSummary RunOptionFaultStress(
    const OptionFaultStressArguments& arguments, const StressBuildIdentity& identity,
    std::span<const OptionMatrixCell> option_cells, std::span<const FaultScenario> fault_scenarios,
    OptionFaultStressObserver* observer = nullptr);
[[nodiscard]] std::string OptionFaultStressUsage();

}  // namespace modern_leveldb::stress

#endif  // MODERN_LEVELDB_STRESS_OPTION_FAULT_STRESS_H_
