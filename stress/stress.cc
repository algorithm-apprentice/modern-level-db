#include "stress/stress.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "modern_leveldb/db.h"

namespace modern_leveldb::stress {
namespace {

constexpr std::string_view RunFormat = "modern-leveldb-stress-run-v1";
constexpr std::string_view OperationFormat = "modern-leveldb-stress-operations-v1";
constexpr std::string_view StateFormat = "modern-leveldb-stress-state-v1";
constexpr std::string_view ResultFormat = "modern-leveldb-stress-result-v1";

std::string OperationName(test_support::ModelOperationKind kind) {
    using test_support::ModelOperationKind;
    switch (kind) {
        case ModelOperationKind::Put:
            return "put";
        case ModelOperationKind::Delete:
            return "delete";
        case ModelOperationKind::Batch:
            return "batch";
        case ModelOperationKind::Snapshot:
            return "snapshot";
        case ModelOperationKind::Release:
            return "release";
        case ModelOperationKind::Read:
            return "read";
        case ModelOperationKind::Reopen:
            return "reopen";
        case ModelOperationKind::Scan:
            return "scan";
    }
    throw std::invalid_argument("unknown model operation");
}

std::string DescribeOperation(const test_support::ModelOperation& operation) {
    using test_support::ModelOperationKind;
    std::string result = std::to_string(operation.index) + "\t" + OperationName(operation.kind);
    switch (operation.kind) {
        case ModelOperationKind::Put:
            result +=
                "\tkey=" + StressHex(operation.key) + "\tvalue=" + StressHex(operation.value);
            break;
        case ModelOperationKind::Delete:
            result += "\tkey=" + StressHex(operation.key);
            break;
        case ModelOperationKind::Batch:
            result += "\tsync=" + std::to_string(operation.sync);
            for (const auto& [key, value] : operation.batch) {
                result += "\tentry=";
                if (value.has_value()) {
                    result += "put:" + StressHex(key) + ":" + StressHex(*value);
                } else {
                    result += "delete:" + StressHex(key);
                }
            }
            break;
        case ModelOperationKind::Snapshot:
        case ModelOperationKind::Release:
            result += "\tslot=" + std::to_string(operation.slot);
            break;
        case ModelOperationKind::Read:
            result += "\tkey=" + StressHex(operation.key);
            result += operation.use_snapshot
                          ? "\tsnapshot=" + std::to_string(operation.slot)
                          : "\tsnapshot=latest";
            break;
        case ModelOperationKind::Reopen:
        case ModelOperationKind::Scan:
            break;
    }
    result.push_back('\n');
    return result;
}

void WriteRun(const std::filesystem::path& output, const StressArguments& arguments,
              const StressBuildIdentity& identity) {
    const std::string text =
        "format=" + std::string(RunFormat) + "\nrevision=" + identity.revision +
        "\ndirty=" + (identity.dirty ? "true" : "false") +
        "\nseed=" + std::to_string(arguments.seed) +
        "\noperations=" + std::to_string(arguments.operations) +
        "\ncompression=" + StressCompressionName(arguments.compression) +
        "\nwrite_buffer_size=" + std::to_string(StressWriteBufferSize) +
        "\nbloom_bits_per_key=" + std::to_string(StressBloomBitsPerKey) + "\n";
    WriteStressText(output / "run.txt", text);
}

void WriteFinalState(const std::filesystem::path& output, const test_support::Model& model) {
    CheckedOutput state(output / "final-state.txt");
    state.Write("format=" + std::string(StateFormat) + "\n");
    for (const auto& [key, value] : model) {
        state.Write(StressHex(key) + "\t" + StressHex(value) + "\n");
    }
}

void WriteCompletion(const std::filesystem::path& output, const StressSummary& summary) {
    const std::string text =
        "format=" + std::string(ResultFormat) + "\noperations=" +
        std::to_string(summary.operations) + "\nentries=" + std::to_string(summary.entries) + "\n";
    WriteStressText(output / "completion.txt", text);
}

void WriteFailure(const std::filesystem::path& output, std::string_view message) {
    const std::string text =
        "format=" + std::string(ResultFormat) +
        "\ncategory=runtime\nmessage=" + SanitizeStressMessage(message) + "\n";
    WriteStressText(output / "failure.txt", text);
}

void VerifyFinalState(test_support::ModernClient& database, const test_support::Model& model) {
    database.Reopen();
    test_support::RequireModelEqual(database.Scan(false),
                                    test_support::ExpectedEntries(model, false),
                                    "final reopened forward scan differs from model");
    test_support::RequireModelEqual(database.Scan(true), test_support::ExpectedEntries(model, true),
                                    "final reopened reverse scan differs from model");
}

}  // namespace

StressArguments ParseStressArguments(std::span<const std::filesystem::path> arguments) {
    StressArguments result;
    if (arguments.size() == 1 && arguments.front() == std::filesystem::path("--help")) {
        result.help = true;
        return result;
    }
    bool have_database = false;
    bool have_output = false;
    bool have_seed = false;
    bool have_operations = false;
    bool have_compression = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::filesystem::path& option = arguments[index];
        if (option == std::filesystem::path("--help")) {
            throw std::invalid_argument("--help cannot be combined with stress options");
        }
        if (index + 1 >= arguments.size()) {
            throw std::invalid_argument("stress option requires a value");
        }
        const std::filesystem::path& value = arguments[++index];
        if (option == std::filesystem::path("--db")) {
            if (have_database) {
                throw std::invalid_argument("--db was provided more than once");
            }
            if (value.empty()) {
                throw std::invalid_argument("--db path is empty");
            }
            result.database = value;
            have_database = true;
        } else if (option == std::filesystem::path("--output")) {
            if (have_output) {
                throw std::invalid_argument("--output was provided more than once");
            }
            if (value.empty()) {
                throw std::invalid_argument("--output path is empty");
            }
            result.output = value;
            have_output = true;
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
            const std::uint64_t operations =
                ParseStressUnsigned(value.string(), "--operations");
            if (operations == 0 || operations > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument("--operations must fit size_t and be positive");
            }
            result.operations = static_cast<std::size_t>(operations);
            have_operations = true;
        } else if (option == std::filesystem::path("--compression")) {
            if (have_compression) {
                throw std::invalid_argument("--compression was provided more than once");
            }
            result.compression = ParseStressCompression(value.string());
            have_compression = true;
        } else {
            throw std::invalid_argument("unknown stress option");
        }
    }
    if (!have_database || !have_output) {
        throw std::invalid_argument("--db and --output are required");
    }
    return result;
}

StressSummary RunStress(const StressArguments& arguments, const StressBuildIdentity& identity,
                        StressObserver* observer) {
    const PreparedStressPaths paths = PrepareStressPaths(arguments.database, arguments.output);
    StressArguments normalized = arguments;
    normalized.database = paths.database;
    normalized.output = paths.output;
    WriteRun(paths.output, normalized, identity);
    CheckedOutput operation_log(paths.output / "operations.log");
    operation_log.Write("format=" + std::string(OperationFormat) + "\n");

    try {
        test_support::ModernClient database(paths.database,
                                            StressDatabaseOptions(arguments.compression));
        const test_support::Model model = test_support::RunModelObserved(
            arguments.seed, arguments.operations,
            [&](const test_support::ModelOperation& operation) {
                operation_log.Write(DescribeOperation(operation));
                if (observer != nullptr) {
                    observer->BeforeOperation(operation);
                }
            },
            database);
        VerifyFinalState(database, model);
        const StressSummary summary{arguments.operations, model.size()};
        WriteFinalState(paths.output, model);
        WriteCompletion(paths.output, summary);
        return summary;
    } catch (const std::exception& error) {
        try {
            WriteFailure(paths.output, error.what());
        } catch (const std::exception& artifact_error) {
            throw std::runtime_error(std::string(error.what()) +
                                     "; cannot write failure artifact: " + artifact_error.what());
        }
        throw;
    } catch (...) {
        try {
            WriteFailure(paths.output, "unknown stress failure");
        } catch (const std::exception& artifact_error) {
            throw std::runtime_error("unknown stress failure; cannot write failure artifact: " +
                                     std::string(artifact_error.what()));
        }
        throw;
    }
}

std::string StressUsage() {
    return "usage: modern_leveldb_stress --db DISPOSABLE_DATABASE_DIRECTORY "
           "--output NEW_ARTIFACT_DIRECTORY [--seed N] [--operations N] "
           "[--compression none|snappy|zstd]\n"
           "Both paths must be absent, distinct, non-nested, and used only for disposable local "
           "stress data.\n";
}

}  // namespace modern_leveldb::stress
