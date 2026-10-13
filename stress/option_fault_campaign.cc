#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "modern_leveldb/base/result.h"
#include "stress/option_fault_stress.h"
#include "stress/stress_support.h"

namespace modern_leveldb::stress {
namespace {

constexpr std::string_view RunFormat = "modern-leveldb-option-fault-run-v1";
constexpr std::string_view CellsFormat = "modern-leveldb-option-fault-cells-v1";
constexpr std::string_view FaultsFormat = "modern-leveldb-option-fault-faults-v1";
constexpr std::string_view LivenessFormat = "modern-leveldb-option-fault-liveness-v1";
constexpr std::string_view ResultFormat = "modern-leveldb-option-fault-result-v1";

struct CampaignPaths {
    std::filesystem::path workspace;
    std::filesystem::path databases;
    std::filesystem::path artifacts;
};

std::size_t ParsePositiveSize(const std::filesystem::path& value, std::string_view option) {
    const std::uint64_t parsed = ParseStressUnsigned(value.string(), option);
    if (parsed == 0 || parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string(option) + " must fit size_t and be positive");
    }
    return static_cast<std::size_t>(parsed);
}

void CreateDirectory(const std::filesystem::path& path, std::string_view name) {
    std::error_code error;
    if (!std::filesystem::create_directory(path, error)) {
        if (error) {
            throw std::runtime_error("cannot create " + std::string(name) +
                                     " directory: " + error.message());
        }
        throw std::runtime_error(std::string(name) + " directory was not created");
    }
}

CampaignPaths PrepareCampaignPaths(const std::filesystem::path& requested_workspace) {
    CampaignPaths paths;
    paths.workspace = PrepareStressWorkspace(requested_workspace);
    paths.artifacts = paths.workspace / "artifacts";
    CreateDirectory(paths.artifacts, "artifact");
    paths.databases = paths.workspace / "databases";
    CreateDirectory(paths.databases, "database");
    return paths;
}

std::string Boolean(bool value) { return value ? "true" : "false"; }

std::string RecoveredName(const std::optional<RecoveredBatchState>& recovered) {
    if (!recovered.has_value()) {
        return "none";
    }
    return *recovered == RecoveredBatchState::Old ? "old" : "new";
}

void WriteRun(const CampaignPaths& paths, const OptionFaultStressArguments& arguments,
              const StressBuildIdentity& identity, std::size_t option_cells,
              std::size_t fault_scenarios) {
    const std::string text =
        "format=" + std::string(RunFormat) + "\nrevision=" + identity.revision +
        "\ndirty=" + Boolean(identity.dirty) + "\nseed=" + std::to_string(arguments.seed) +
        "\noperations=" + std::to_string(arguments.operations) +
        "\nliveness_writes=" + std::to_string(arguments.liveness_writes) +
        "\noption_cells=" + std::to_string(option_cells) +
        "\nfault_scenarios=" + std::to_string(fault_scenarios) + "\n";
    WriteStressText(paths.artifacts / "run.txt", text);
}

void WriteLiveness(const CampaignPaths& paths, const LivenessScenarioSummary& summary) {
    const std::string text = "format=" + std::string(LivenessFormat) +
                             "\nseed=" + std::to_string(summary.seed) +
                             "\nwrites=" + std::to_string(summary.writes) +
                             "\nbackground_tasks=" + std::to_string(summary.background_tasks) +
                             "\ntable_outputs=" + std::to_string(summary.table_outputs) +
                             "\npoint_reads=" + std::to_string(summary.point_reads) +
                             "\nscans=" + std::to_string(summary.scans) +
                             "\nentries=" + std::to_string(summary.entries) +
                             "\nfingerprint=" + summary.fingerprint + "\n";
    WriteStressText(paths.artifacts / "liveness.txt", text);
}

void WriteCompletion(const CampaignPaths& paths, const OptionFaultStressSummary& summary) {
    const std::string text =
        "format=" + std::string(ResultFormat) +
        "\noption_cells=" + std::to_string(summary.option_cells) +
        "\nfault_scenarios=" + std::to_string(summary.fault_scenarios) +
        "\noperations_per_cell=" + std::to_string(summary.operations_per_cell) +
        "\noption_entries=" + std::to_string(summary.option_entries) +
        "\nliveness_writes=" + std::to_string(summary.liveness.writes) +
        "\nliveness_entries=" + std::to_string(summary.liveness.entries) + "\n";
    WriteStressText(paths.artifacts / "completion.txt", text);
}

void WriteFailure(const CampaignPaths& paths, std::string_view message) {
    const std::string text = "format=" + std::string(ResultFormat) +
                             "\ncategory=runtime\nmessage=" + SanitizeStressMessage(message) + "\n";
    WriteStressText(paths.artifacts / "failure.txt", text);
}

}  // namespace

OptionFaultStressArguments ParseOptionFaultStressArguments(
    std::span<const std::filesystem::path> arguments) {
    OptionFaultStressArguments result;
    if (arguments.size() == 1 && arguments.front() == std::filesystem::path("--help")) {
        result.help = true;
        return result;
    }

    bool have_workspace = false;
    bool have_seed = false;
    bool have_operations = false;
    bool have_liveness_writes = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::filesystem::path& option = arguments[index];
        if (option == std::filesystem::path("--help")) {
            throw std::invalid_argument(
                "--help cannot be combined with option-fault stress options");
        }
        if (index + 1 >= arguments.size()) {
            throw std::invalid_argument("option-fault stress option requires a value");
        }
        const std::filesystem::path& value = arguments[++index];
        if (option == std::filesystem::path("--workspace")) {
            if (have_workspace) {
                throw std::invalid_argument("--workspace was provided more than once");
            }
            if (value.empty()) {
                throw std::invalid_argument("--workspace path is empty");
            }
            result.workspace = value;
            have_workspace = true;
        } else if (option == std::filesystem::path("--seed")) {
            if (have_seed) {
                throw std::invalid_argument("--seed was provided more than once");
            }
            result.seed = ParseStressUnsigned(value.string(), "--seed");
            have_seed = true;
        } else if (option == std::filesystem::path("--operations")) {
            if (have_operations) {
                throw std::invalid_argument("--operations was provided more than once");
            }
            result.operations = ParsePositiveSize(value, "--operations");
            have_operations = true;
        } else if (option == std::filesystem::path("--liveness-writes")) {
            if (have_liveness_writes) {
                throw std::invalid_argument("--liveness-writes was provided more than once");
            }
            result.liveness_writes = ParsePositiveSize(value, "--liveness-writes");
            have_liveness_writes = true;
        } else {
            throw std::invalid_argument("unknown option-fault stress option");
        }
    }
    if (!have_workspace) {
        throw std::invalid_argument("--workspace is required");
    }
    return result;
}

OptionFaultStressSummary RunOptionFaultStress(const OptionFaultStressArguments& arguments,
                                              const StressBuildIdentity& identity,
                                              std::span<const OptionMatrixCell> option_cells,
                                              std::span<const FaultScenario> fault_scenarios,
                                              OptionFaultStressObserver* observer) {
    const CampaignPaths paths = PrepareCampaignPaths(arguments.workspace);
    WriteRun(paths, arguments, identity, option_cells.size(), fault_scenarios.size());
    CheckedOutput cells(paths.artifacts / "cells.tsv");
    cells.Write("format=" + std::string(CellsFormat) + "\n");
    cells.Write("index\tid\tseed\toperations\tentries\tfingerprint\n");
    CheckedOutput faults(paths.artifacts / "faults.tsv");
    faults.Write("format=" + std::string(FaultsFormat) + "\n");
    faults.Write(
        "index\tname\toperation\terror_code\terror_message\tsticky\t"
        "later_write_reused_error_without_io\tretry_succeeded\t"
        "recovered_batch\n");

    try {
        OptionFaultStressSummary summary;
        summary.operations_per_cell = arguments.operations;
        for (std::size_t index = 0; index < option_cells.size(); ++index) {
            const OptionMatrixCell& cell = option_cells[index];
            if (observer != nullptr) {
                observer->BeforeOptionCell(index, cell);
            }
            const OptionMatrixCellSummary cell_summary =
                RunOptionMatrixCell(paths.databases / OptionMatrixCellId(cell), cell,
                                    arguments.seed, arguments.operations);
            cells.Write(std::to_string(index) + "\t" + cell_summary.id + "\t" +
                        std::to_string(cell_summary.seed) + "\t" +
                        std::to_string(cell_summary.operations) + "\t" +
                        std::to_string(cell_summary.entries) + "\t" + cell_summary.fingerprint +
                        "\n");
            ++summary.option_cells;
            summary.option_entries += cell_summary.entries;
        }

        for (std::size_t index = 0; index < fault_scenarios.size(); ++index) {
            const FaultScenario scenario = fault_scenarios[index];
            if (observer != nullptr) {
                observer->BeforeFaultScenario(index, scenario);
            }
            const FaultScenarioSummary fault = RunFaultScenario(scenario);
            faults.Write(std::to_string(index) + "\t" + fault.name + "\t" +
                         SanitizeStressMessage(fault.injected_operation) + "\t" +
                         std::string(ErrorCodeName(fault.error_code)) + "\t" +
                         SanitizeStressMessage(fault.error_message) + "\t" + Boolean(fault.sticky) +
                         "\t" + Boolean(fault.later_write_reused_error_without_io) + "\t" +
                         Boolean(fault.retry_succeeded) + "\t" +
                         RecoveredName(fault.recovered_batch) + "\n");
            ++summary.fault_scenarios;
        }

        if (observer != nullptr) {
            observer->BeforeLiveness();
        }
        summary.liveness = RunLivenessScenario(arguments.seed, arguments.liveness_writes);
        WriteLiveness(paths, summary.liveness);
        WriteCompletion(paths, summary);
        return summary;
    } catch (const std::exception& error) {
        try {
            WriteFailure(paths, error.what());
        } catch (const std::exception& artifact_error) {
            throw std::runtime_error(std::string(error.what()) +
                                     "; cannot write failure artifact: " + artifact_error.what());
        }
        throw;
    } catch (...) {
        try {
            WriteFailure(paths, "unknown option-fault stress failure");
        } catch (const std::exception& artifact_error) {
            throw std::runtime_error(
                "unknown option-fault stress failure; cannot write failure "
                "artifact: " +
                std::string(artifact_error.what()));
        }
        throw;
    }
}

OptionFaultStressSummary RunOptionFaultStress(const OptionFaultStressArguments& arguments,
                                              const StressBuildIdentity& identity,
                                              OptionFaultStressObserver* observer) {
    const std::vector<OptionMatrixCell> option_cells = EnumerateOptionMatrix();
    const std::vector<FaultScenario> fault_scenarios = EnumerateFaultScenarios();
    return RunOptionFaultStress(arguments, identity, option_cells, fault_scenarios, observer);
}

std::string OptionFaultStressUsage() {
    return "usage: modern_leveldb_option_fault_stress "
           "--workspace NEW_DISPOSABLE_DIRECTORY [--seed N] "
           "[--operations N] [--liveness-writes N]\n"
           "The workspace must be absent with an existing parent and is used "
           "only for disposable local stress data. This long campaign is not "
           "a hosted CI command.\n";
}

}  // namespace modern_leveldb::stress
