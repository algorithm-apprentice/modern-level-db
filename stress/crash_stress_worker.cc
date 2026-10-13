#include "stress/crash_stress_worker.h"

#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "modern_leveldb/db.h"
#include "modern_leveldb/write_batch.h"
#include "stress/stress_support.h"

namespace modern_leveldb::stress {
namespace {

constexpr std::string_view JournalFormat = "format=modern-leveldb-crash-journal-v1";
constexpr std::string_view Committed = "committed";

template <typename T>
T Take(Result<T> result) {
    if (!result.has_value()) {
        throw std::runtime_error(result.error().ToString());
    }
    return std::move(*result);
}

void Check(Status status) {
    if (!status.has_value()) {
        throw std::runtime_error(status.error().ToString());
    }
}

std::optional<std::string> ReadValue(Database& database, std::string_view key) {
    const auto value = Take(database.Get(AsBytes(key)));
    if (!value.has_value()) {
        return std::nullopt;
    }
    return std::string(AsStringView(*value));
}

std::map<std::string, std::string> Scan(Database& database) {
    Iterator iterator = Take(database.NewIterator());
    Check(iterator.SeekToFirst());
    std::map<std::string, std::string> result;
    while (iterator.valid()) {
        const auto [entry, inserted] =
            result.emplace(AsStringView(iterator.key()), AsStringView(iterator.value()));
        static_cast<void>(entry);
        if (!inserted) {
            throw std::runtime_error("crash verification iterator returned a duplicate key");
        }
        Check(iterator.Next());
    }
    return result;
}

CrashStressWorkerMode ParseMode(std::string_view mode) {
    if (mode == "write-hold") {
        return CrashStressWorkerMode::WriteHold;
    }
    if (mode == "inflight-hold") {
        return CrashStressWorkerMode::InflightHold;
    }
    if (mode == "verify-prefix") {
        return CrashStressWorkerMode::VerifyPrefix;
    }
    if (mode == "verify-optional") {
        return CrashStressWorkerMode::VerifyOptional;
    }
    if (mode == "initialize") {
        return CrashStressWorkerMode::Initialize;
    }
    if (mode == "identity") {
        return CrashStressWorkerMode::Identity;
    }
    throw std::invalid_argument("unknown crash stress worker mode");
}

bool IsWriteMode(CrashStressWorkerMode mode) {
    return mode == CrashStressWorkerMode::WriteHold ||
           mode == CrashStressWorkerMode::InflightHold;
}

Options CrashDatabaseOptions(Compression compression, bool create, bool reject_existing) {
    Options options = StressDatabaseOptions(compression);
    options.create_if_missing = create;
    options.error_if_exists = reject_existing;
    return options;
}

std::string ReadWholeFile(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error || file_size > std::numeric_limits<std::size_t>::max() ||
        file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("cannot size crash acknowledgement journal");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read crash acknowledgement journal");
    }
    std::string result(static_cast<std::size_t>(file_size), '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (input.gcount() != static_cast<std::streamsize>(result.size())) {
        throw std::runtime_error("cannot read complete crash acknowledgement journal");
    }
    return result;
}

std::size_t ParseIndex(std::string_view text, std::string_view context) {
    const std::uint64_t value = ParseStressUnsigned(text, context);
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string(context) + " exceeds size_t");
    }
    return static_cast<std::size_t>(value);
}

void VerifyCompleteBatch(Database& database, std::uint64_t seed, std::size_t index) {
    const auto latest = ReadValue(database, CrashStressLatestKey);
    const auto marker = ReadValue(database, CrashStressMarkerKey(index));
    if (latest != std::optional<std::string>(CrashStressValue(seed, index)) ||
        marker != std::optional<std::string>(std::string(Committed))) {
        throw std::runtime_error("crash stress batch is absent, partial, or malformed");
    }
}

}  // namespace

CrashStressWorkerArguments ParseCrashStressWorkerArguments(
    std::span<const std::filesystem::path> arguments) {
    CrashStressWorkerArguments result;
    if (arguments.size() == 1 && arguments.front() == std::filesystem::path("--help")) {
        result.help = true;
        return result;
    }
    bool have_mode = false;
    bool have_database = false;
    bool have_journal = false;
    bool have_seed = false;
    bool have_index = false;
    bool have_compression = false;
    for (std::size_t position = 0; position < arguments.size(); ++position) {
        const std::filesystem::path& option = arguments[position];
        if (position + 1 >= arguments.size()) {
            throw std::invalid_argument("crash stress worker option requires a value");
        }
        const std::filesystem::path& value = arguments[++position];
        if (option == std::filesystem::path("--mode")) {
            if (have_mode) {
                throw std::invalid_argument("--mode was provided more than once");
            }
            result.mode = ParseMode(value.string());
            have_mode = true;
        } else if (option == std::filesystem::path("--db")) {
            if (have_database || value.empty()) {
                throw std::invalid_argument("--db must be provided once with a nonempty path");
            }
            result.database = value;
            have_database = true;
        } else if (option == std::filesystem::path("--journal")) {
            if (have_journal || value.empty()) {
                throw std::invalid_argument("--journal must be provided once with a nonempty path");
            }
            result.journal = value;
            have_journal = true;
        } else if (option == std::filesystem::path("--seed")) {
            if (have_seed) {
                throw std::invalid_argument("--seed was provided more than once");
            }
            result.seed = ParseStressUnsigned(value.string(), "--seed");
            have_seed = true;
        } else if (option == std::filesystem::path("--index")) {
            if (have_index) {
                throw std::invalid_argument("--index was provided more than once");
            }
            result.index = ParseIndex(value.string(), "--index");
            have_index = true;
        } else if (option == std::filesystem::path("--compression")) {
            if (have_compression) {
                throw std::invalid_argument("--compression was provided more than once");
            }
            result.compression = ParseStressCompression(value.string());
            have_compression = true;
        } else {
            throw std::invalid_argument("unknown crash stress worker option");
        }
    }
    if (!have_mode) {
        throw std::invalid_argument("--mode is required");
    }
    if (result.mode == CrashStressWorkerMode::Identity) {
        if (arguments.size() != 2) {
            throw std::invalid_argument("identity mode accepts no other options");
        }
        return result;
    }
    if (!have_database) {
        throw std::invalid_argument("--db is required");
    }
    if (IsWriteMode(result.mode) || result.mode == CrashStressWorkerMode::VerifyOptional) {
        if (!have_index || have_journal) {
            throw std::invalid_argument("selected mode requires --index and no --journal");
        }
    } else if (result.mode == CrashStressWorkerMode::VerifyPrefix) {
        if (!have_journal || have_index) {
            throw std::invalid_argument("verify-prefix requires --journal and no --index");
        }
    } else if (result.mode == CrashStressWorkerMode::Initialize) {
        if (have_journal || have_index) {
            throw std::invalid_argument("initialize accepts no --journal or --index");
        }
    }
    return result;
}

std::string CrashStressValue(std::uint64_t seed, std::size_t index) {
    const std::string header =
        "seed=" + std::to_string(seed) + ";index=" + std::to_string(index) + ";";
    if (header.size() > CrashStressValueSize) {
        throw std::invalid_argument("crash stress value header exceeds its fixed size");
    }
    const char fill = static_cast<char>((seed ^ static_cast<std::uint64_t>(index)) % 251U + 1U);
    std::string result = header;
    result.append(CrashStressValueSize - result.size(), fill);
    return result;
}

std::string CrashStressMarkerKey(std::size_t index) {
    return "crash/marker/" + std::to_string(index);
}

std::string CrashStressAcknowledgement(std::size_t index) {
    return "ack\t" + std::to_string(index) + "\n";
}

std::string CrashStressReadiness(std::size_t index) {
    return "ready\t" + std::to_string(index) + "\n";
}

CrashStressJournal ParseCrashStressJournal(const std::filesystem::path& path) {
    const std::string contents = ReadWholeFile(path);
    std::size_t start = 0;
    std::size_t line_index = 0;
    while (start < contents.size()) {
        const std::size_t end = contents.find('\n', start);
        if (end == std::string::npos) {
            throw std::runtime_error("crash acknowledgement journal has an unterminated line");
        }
        const std::string_view line(contents.data() + start, end - start);
        if (line_index == 0) {
            if (line != JournalFormat) {
                throw std::runtime_error("crash acknowledgement journal has the wrong format");
            }
        } else {
            constexpr std::string_view Prefix = "ack\t";
            if (!line.starts_with(Prefix)) {
                throw std::runtime_error("crash acknowledgement journal has an unknown record");
            }
            std::size_t parsed = 0;
            try {
                parsed = ParseIndex(line.substr(Prefix.size()), "journal index");
            } catch (const std::invalid_argument&) {
                throw std::runtime_error(
                    "crash acknowledgement journal has a malformed index");
            }
            if (parsed != line_index - 1) {
                throw std::runtime_error(
                    "crash acknowledgement journal is not a contiguous prefix");
            }
        }
        ++line_index;
        start = end + 1;
    }
    if (line_index == 0) {
        throw std::runtime_error("crash acknowledgement journal is empty");
    }
    return {line_index - 1};
}

void InitializeCrashStressDatabase(const std::filesystem::path& database,
                                   Compression compression) {
    static_cast<void>(
        Take(Database::Open(CrashDatabaseOptions(compression, true, true), database)));
}

void WriteCrashStressBatch(const std::filesystem::path& database, std::uint64_t seed,
                           std::size_t index, Compression compression) {
    Database handle = OpenCrashStressDatabase(database, compression, index == 0);
    ApplyCrashStressBatch(handle, seed, index);
}

Database OpenCrashStressDatabase(const std::filesystem::path& database,
                                 Compression compression, bool create) {
    auto opened =
        Database::Open(CrashDatabaseOptions(compression, create, create), database);
    return Take(std::move(opened));
}

void ApplyCrashStressBatch(Database& database, std::uint64_t seed, std::size_t index) {
    const std::string value = CrashStressValue(seed, index);
    WriteBatch batch;
    Check(batch.Put(AsBytes(CrashStressLatestKey), AsBytes(value)));
    Check(batch.Put(AsBytes(CrashStressMarkerKey(index)), AsBytes(Committed)));
    Check(database.Write(batch, {.sync = true}));
}

std::size_t VerifyCrashStressPrefix(const std::filesystem::path& database,
                                    const std::filesystem::path& journal,
                                    std::uint64_t seed, Compression compression) {
    const CrashStressJournal parsed = ParseCrashStressJournal(journal);
    if (parsed.acknowledged == 0) {
        return 0;
    }
    Database handle =
        Take(Database::Open(CrashDatabaseOptions(compression, false, false), database));
    for (std::size_t index = 0; index != parsed.acknowledged; ++index) {
        const auto marker = ReadValue(handle, CrashStressMarkerKey(index));
        if (marker != std::optional<std::string>(std::string(Committed))) {
            throw std::runtime_error("acknowledged crash marker is missing");
        }
    }
    const auto latest = ReadValue(handle, CrashStressLatestKey);
    if (latest !=
        std::optional<std::string>(CrashStressValue(seed, parsed.acknowledged - 1))) {
        throw std::runtime_error("latest acknowledged crash value is missing");
    }
    const std::map<std::string, std::string> entries = Scan(handle);
    if (entries.size() != parsed.acknowledged + 1) {
        throw std::runtime_error("recovered crash database has unexpected visible keys");
    }
    return parsed.acknowledged;
}

OptionalCrashBatchState VerifyOptionalCrashStressBatch(
    const std::filesystem::path& database, std::uint64_t seed, std::size_t index,
    Compression compression) {
    std::error_code exists_error;
    if (!std::filesystem::exists(database, exists_error)) {
        if (exists_error) {
            throw std::runtime_error("cannot inspect optional crash database: " +
                                     exists_error.message());
        }
        return OptionalCrashBatchState::Absent;
    }
    auto opened = Database::Open(CrashDatabaseOptions(compression, false, false), database);
    if (!opened.has_value()) {
        throw std::runtime_error(opened.error().ToString());
    }
    const auto latest = ReadValue(*opened, CrashStressLatestKey);
    const auto marker = ReadValue(*opened, CrashStressMarkerKey(index));
    if (!latest.has_value() && !marker.has_value()) {
        return OptionalCrashBatchState::Absent;
    }
    VerifyCompleteBatch(*opened, seed, index);
    return OptionalCrashBatchState::Complete;
}

std::string CrashStressWorkerUsage() {
    return "usage: modern_leveldb_crash_stress_worker --mode "
           "write-hold|inflight-hold|verify-prefix|verify-optional|initialize|identity "
           "[--db PATH] [--journal PATH] [--seed N] [--index N] "
           "[--compression none|snappy|zstd]\n";
}

}  // namespace modern_leveldb::stress
