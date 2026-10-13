#include "stress/stress.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include "modern_leveldb/db.h"

namespace modern_leveldb::stress {
namespace {

constexpr std::string_view RunFormat = "modern-leveldb-stress-run-v1";
constexpr std::string_view OperationFormat = "modern-leveldb-stress-operations-v1";
constexpr std::string_view StateFormat = "modern-leveldb-stress-state-v1";
constexpr std::string_view ResultFormat = "modern-leveldb-stress-result-v1";
constexpr std::size_t WriteBufferSize = 64U * 1024U;
constexpr std::uint32_t BloomBitsPerKey = 10;

class CheckedOutput final {
public:
    explicit CheckedOutput(const std::filesystem::path& path)
        : path_(path), stream_(path, std::ios::binary | std::ios::trunc) {
        if (!stream_) {
            throw std::runtime_error("cannot create stress artifact");
        }
    }

    void Write(std::string_view text) {
        stream_.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream_.flush();
        if (!stream_) {
            throw std::runtime_error("cannot write stress artifact");
        }
    }

private:
    std::filesystem::path path_;
    std::ofstream stream_;
};

struct PreparedPaths {
    std::filesystem::path database;
    std::filesystem::path output;
};

std::string CompressionName(Compression compression) {
    switch (compression) {
        case Compression::None:
            return "none";
        case Compression::Snappy:
            return "snappy";
        case Compression::Zstd:
            return "zstd";
    }
    throw std::invalid_argument("unsupported stress compression");
}

std::uint64_t ParseUnsigned(std::string_view text, std::string_view option) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument(std::string(option) + " requires an unsigned integer");
    }
    return value;
}

bool StartsWithPath(const std::filesystem::path& path, const std::filesystem::path& prefix) {
    auto current = path.begin();
    auto expected = prefix.begin();
    while (current != path.end() && expected != prefix.end() && *current == *expected) {
        ++current;
        ++expected;
    }
    return expected == prefix.end();
}

std::filesystem::path NormalizeAbsentPath(const std::filesystem::path& path,
                                          std::string_view name) {
    if (path.empty()) {
        throw std::runtime_error(std::string(name) + " path is empty");
    }
    std::error_code error;
    const std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
    if (error) {
        throw std::runtime_error("cannot normalize " + std::string(name) + " path: " +
                                 error.message());
    }
    if (std::filesystem::exists(normalized, error)) {
        throw std::runtime_error(std::string(name) + " path already exists");
    }
    if (error) {
        throw std::runtime_error("cannot inspect " + std::string(name) + " path: " +
                                 error.message());
    }
    const std::filesystem::path parent = normalized.parent_path();
    if (!std::filesystem::is_directory(parent, error)) {
        if (error) {
            throw std::runtime_error("cannot inspect " + std::string(name) + " parent: " +
                                     error.message());
        }
        throw std::runtime_error(std::string(name) + " parent directory does not exist");
    }
    return normalized;
}

PreparedPaths PreparePaths(const StressArguments& arguments) {
    PreparedPaths result{
        NormalizeAbsentPath(arguments.database, "database"),
        NormalizeAbsentPath(arguments.output, "output"),
    };
    if (StartsWithPath(result.database, result.output) ||
        StartsWithPath(result.output, result.database)) {
        throw std::runtime_error("database and output paths must be distinct and non-nested");
    }
    std::error_code error;
    if (!std::filesystem::create_directory(result.output, error)) {
        if (error) {
            throw std::runtime_error("cannot create stress output directory: " + error.message());
        }
        throw std::runtime_error("stress output directory was not created");
    }
    return result;
}

char HexDigit(unsigned value) {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
}

std::string Hex(std::string_view bytes) {
    std::string encoded(bytes.size() * 2, '\0');
    std::size_t output = 0;
    for (const unsigned char byte : bytes) {
        encoded[output++] = HexDigit(byte >> 4U);
        encoded[output++] = HexDigit(byte & 0x0fU);
    }
    return encoded;
}

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
            result += "\tkey=" + Hex(operation.key) + "\tvalue=" + Hex(operation.value);
            break;
        case ModelOperationKind::Delete:
            result += "\tkey=" + Hex(operation.key);
            break;
        case ModelOperationKind::Batch:
            result += "\tsync=" + std::to_string(operation.sync);
            for (const auto& [key, value] : operation.batch) {
                result += "\tentry=";
                if (value.has_value()) {
                    result += "put:" + Hex(key) + ":" + Hex(*value);
                } else {
                    result += "delete:" + Hex(key);
                }
            }
            break;
        case ModelOperationKind::Snapshot:
        case ModelOperationKind::Release:
            result += "\tslot=" + std::to_string(operation.slot);
            break;
        case ModelOperationKind::Read:
            result += "\tkey=" + Hex(operation.key);
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

std::string SanitizeMessage(std::string_view message) {
    std::string result(message);
    for (char& character : result) {
        if (character == '\n' || character == '\r' || character == '\t') {
            character = ' ';
        }
    }
    return result;
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    CheckedOutput output(path);
    output.Write(text);
}

void WriteRun(const std::filesystem::path& output, const StressArguments& arguments,
              const StressBuildIdentity& identity) {
    const std::string text =
        "format=" + std::string(RunFormat) + "\nrevision=" + identity.revision +
        "\ndirty=" + (identity.dirty ? "true" : "false") +
        "\nseed=" + std::to_string(arguments.seed) +
        "\noperations=" + std::to_string(arguments.operations) +
        "\ncompression=" + CompressionName(arguments.compression) +
        "\nwrite_buffer_size=" + std::to_string(WriteBufferSize) +
        "\nbloom_bits_per_key=" + std::to_string(BloomBitsPerKey) + "\n";
    WriteText(output / "run.txt", text);
}

void WriteFinalState(const std::filesystem::path& output, const test_support::Model& model) {
    CheckedOutput state(output / "final-state.txt");
    state.Write("format=" + std::string(StateFormat) + "\n");
    for (const auto& [key, value] : model) {
        state.Write(Hex(key) + "\t" + Hex(value) + "\n");
    }
}

void WriteCompletion(const std::filesystem::path& output, const StressSummary& summary) {
    const std::string text =
        "format=" + std::string(ResultFormat) + "\noperations=" +
        std::to_string(summary.operations) + "\nentries=" + std::to_string(summary.entries) + "\n";
    WriteText(output / "completion.txt", text);
}

void WriteFailure(const std::filesystem::path& output, std::string_view message) {
    const std::string text =
        "format=" + std::string(ResultFormat) + "\nmessage=" + SanitizeMessage(message) + "\n";
    WriteText(output / "failure.txt", text);
}

Options DatabaseOptions(Compression compression) {
    Options options;
    options.create_if_missing = true;
    options.write_buffer_size = WriteBufferSize;
    options.bloom_bits_per_key = BloomBitsPerKey;
    options.compression = compression;
#if defined(_WIN32)
    options.allow_weak_namespace_durability = true;
#endif
    return options;
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
            result.database = value;
            have_database = true;
        } else if (option == std::filesystem::path("--output")) {
            if (have_output) {
                throw std::invalid_argument("--output was provided more than once");
            }
            result.output = value;
            have_output = true;
        } else if (option == std::filesystem::path("--seed")) {
            if (have_seed) {
                throw std::invalid_argument("--seed was provided more than once");
            }
            result.seed = ParseUnsigned(value.string(), "--seed");
            have_seed = true;
        } else if (option == std::filesystem::path("--operations")) {
            if (have_operations) {
                throw std::invalid_argument("--operations was provided more than once");
            }
            const std::uint64_t operations = ParseUnsigned(value.string(), "--operations");
            if (operations == 0 || operations > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument("--operations must fit size_t and be positive");
            }
            result.operations = static_cast<std::size_t>(operations);
            have_operations = true;
        } else if (option == std::filesystem::path("--compression")) {
            if (have_compression) {
                throw std::invalid_argument("--compression was provided more than once");
            }
            const std::string compression = value.string();
            if (compression == "none") {
                result.compression = Compression::None;
            } else if (compression == "snappy") {
                result.compression = Compression::Snappy;
            } else if (compression == "zstd") {
                result.compression = Compression::Zstd;
            } else {
                throw std::invalid_argument("unknown stress compression");
            }
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
    const PreparedPaths paths = PreparePaths(arguments);
    StressArguments normalized = arguments;
    normalized.database = paths.database;
    normalized.output = paths.output;
    WriteRun(paths.output, normalized, identity);
    CheckedOutput operation_log(paths.output / "operations.log");
    operation_log.Write("format=" + std::string(OperationFormat) + "\n");

    try {
        test_support::ModernClient database(paths.database,
                                            DatabaseOptions(arguments.compression));
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
