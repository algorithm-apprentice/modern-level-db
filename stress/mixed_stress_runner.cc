#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "modern_leveldb/db.h"
#include "stress/mixed_stress.h"

namespace modern_leveldb::stress {
namespace {

constexpr std::string_view RunFormat = "modern-leveldb-mixed-stress-run-v1";
constexpr std::string_view OperationFormat = "modern-leveldb-mixed-stress-operations-v1";
constexpr std::string_view EpochFormat = "modern-leveldb-mixed-stress-epochs-v1";
constexpr std::string_view StateFormat = "modern-leveldb-mixed-stress-state-v1";
constexpr std::string_view ResultFormat = "modern-leveldb-mixed-stress-result-v1";
constexpr std::size_t StripeCount = 256;
constexpr std::size_t ScanLimit = 64;

class StartGate final {
public:
    bool Wait() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] { return open_; });
        return !cancelled_;
    }

    void Open() {
        {
            const std::lock_guard lock(mutex_);
            open_ = true;
        }
        condition_.notify_all();
    }

    void Cancel() {
        {
            const std::lock_guard lock(mutex_);
            cancelled_ = true;
            open_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool open_ = false;
    bool cancelled_ = false;
};

class StripedModel final {
public:
    struct Stripe {
        std::mutex mutex;
        std::map<std::string, std::string> entries;
    };

    Stripe& For(std::string_view key) {
        if (key.empty()) {
            throw std::invalid_argument("mixed model key is empty");
        }
        return stripes_[static_cast<unsigned char>(key.front())];
    }

    std::map<std::string, std::string> Snapshot() {
        std::map<std::string, std::string> result;
        for (Stripe& stripe : stripes_) {
            const std::lock_guard lock(stripe.mutex);
            result.insert(stripe.entries.begin(), stripe.entries.end());
        }
        return result;
    }

    std::vector<std::pair<std::string, std::string>> Prefix(std::string_view prefix) {
        if (prefix.size() != 2) {
            throw std::invalid_argument("mixed prefix must contain two bytes");
        }
        Stripe& stripe = stripes_[static_cast<unsigned char>(prefix.front())];
        const std::lock_guard lock(stripe.mutex);
        std::vector<std::pair<std::string, std::string>> result;
        for (auto entry = stripe.entries.lower_bound(std::string(prefix));
             entry != stripe.entries.end() && entry->first.starts_with(prefix); ++entry) {
            result.push_back(*entry);
        }
        return result;
    }

    std::vector<std::pair<std::string, std::string>> Forward(std::string_view target,
                                                             std::size_t limit) {
        std::vector<std::pair<std::string, std::string>> result;
        const std::size_t start = static_cast<unsigned char>(target.front());
        for (std::size_t index = start; index < StripeCount && result.size() != limit; ++index) {
            Stripe& stripe = stripes_[index];
            const std::lock_guard lock(stripe.mutex);
            auto entry = index == start ? stripe.entries.lower_bound(std::string(target))
                                        : stripe.entries.begin();
            while (entry != stripe.entries.end() && result.size() != limit) {
                result.push_back(*entry);
                ++entry;
            }
        }
        return result;
    }

    std::vector<std::pair<std::string, std::string>> Reverse(std::string_view target,
                                                             std::size_t limit) {
        std::vector<std::pair<std::string, std::string>> result;
        const std::size_t start = static_cast<unsigned char>(target.front());
        for (std::size_t cursor = start + 1U; cursor != 0 && result.size() != limit; --cursor) {
            const std::size_t index = cursor - 1U;
            Stripe& stripe = stripes_[index];
            const std::lock_guard lock(stripe.mutex);
            auto entry = index == start ? stripe.entries.upper_bound(std::string(target))
                                        : stripe.entries.end();
            while (entry != stripe.entries.begin() && result.size() != limit) {
                --entry;
                result.push_back(*entry);
            }
        }
        return result;
    }

private:
    std::array<Stripe, StripeCount> stripes_;
};

struct MixedCounters {
    std::array<std::atomic<std::size_t>, 5> operations{};
    std::atomic<std::size_t> fresh_write_plans{0};
};

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

std::optional<std::string> ReadValue(Database& database, std::string_view key,
                                     const ReadOptions& options = {}) {
    const auto value = Take(database.Get(AsBytes(key), options));
    if (!value.has_value()) {
        return std::nullopt;
    }
    return std::string(AsStringView(*value));
}

std::vector<std::pair<std::string, std::string>> ReadForward(
    Iterator& iterator, std::optional<std::string_view> target = {},
    std::optional<std::string_view> prefix = {}, std::size_t limit = 0) {
    if (target.has_value()) {
        Check(iterator.Seek(AsBytes(*target)));
    } else {
        Check(iterator.SeekToFirst());
    }
    std::vector<std::pair<std::string, std::string>> result;
    while (iterator.valid()) {
        const std::string_view key = AsStringView(iterator.key());
        if (prefix.has_value() && !key.starts_with(*prefix)) {
            break;
        }
        if (limit != 0 && result.size() == limit) {
            break;
        }
        result.emplace_back(key, AsStringView(iterator.value()));
        Check(iterator.Next());
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> ReadReverse(
    Iterator& iterator, std::optional<std::string_view> target = {}, std::size_t limit = 0) {
    if (target.has_value()) {
        Check(iterator.Seek(AsBytes(*target)));
        if (iterator.valid()) {
            if (AsStringView(iterator.key()) > *target) {
                Check(iterator.Prev());
            }
        } else {
            Check(iterator.SeekToLast());
        }
    } else {
        Check(iterator.SeekToLast());
    }
    std::vector<std::pair<std::string, std::string>> result;
    while (iterator.valid()) {
        if (limit != 0 && result.size() == limit) {
            break;
        }
        result.emplace_back(AsStringView(iterator.key()), AsStringView(iterator.value()));
        Check(iterator.Prev());
    }
    return result;
}

void CheckPrefix(Database& database, StripedModel& model, std::shared_mutex& quiescence,
                 std::uint64_t slot) {
    const std::unique_lock pause(quiescence);
    const std::string key = MixedKey(slot);
    const std::string prefix(MixedKeyPrefix(key));
    const auto expected = model.Prefix(prefix);
    std::optional<Snapshot> snapshot(Take(database.GetSnapshot()));
    ReadOptions options{.snapshot = &*snapshot};
    Iterator iterator = Take(database.NewIterator(options));
    const auto actual = ReadForward(iterator, prefix, prefix);
    if (actual != expected) {
        throw std::runtime_error("mixed prefix scan differs from model");
    }
}

void CheckIteration(Database& database, StripedModel& model, std::shared_mutex& quiescence,
                    std::uint64_t slot, bool reverse) {
    const std::unique_lock pause(quiescence);
    const std::string target = MixedKey(slot);
    const auto expected =
        reverse ? model.Reverse(target, ScanLimit) : model.Forward(target, ScanLimit);
    std::optional<Snapshot> snapshot(Take(database.GetSnapshot()));
    ReadOptions options{.snapshot = &*snapshot};
    Iterator iterator = Take(database.NewIterator(options));
    const auto actual = reverse ? ReadReverse(iterator, target, ScanLimit)
                                : ReadForward(iterator, target, {}, ScanLimit);
    if (actual != expected) {
        throw std::runtime_error("mixed iterator range differs from model");
    }
}

void CheckPointRead(Database& database, StripedModel& model, std::shared_mutex& quiescence,
                    const std::string& key) {
    const std::shared_lock active(quiescence);
    StripedModel::Stripe& stripe = model.For(key);
    const std::lock_guard lock(stripe.mutex);
    std::optional<std::string> expected;
    if (const auto entry = stripe.entries.find(key); entry != stripe.entries.end()) {
        expected = entry->second;
    }
    if (ReadValue(database, key) != expected) {
        throw std::runtime_error("mixed point read differs from model");
    }
}

void WriteValue(Database& database, StripedModel& model, std::shared_mutex& quiescence,
                const std::string& key, const std::string& value) {
    const std::shared_lock active(quiescence);
    StripedModel::Stripe& stripe = model.For(key);
    const std::lock_guard lock(stripe.mutex);
    Check(database.Put(AsBytes(key), AsBytes(value)));
    stripe.entries[key] = value;
}

void DeleteValue(Database& database, StripedModel& model, std::shared_mutex& quiescence,
                 const std::string& key) {
    const std::shared_lock active(quiescence);
    StripedModel::Stripe& stripe = model.For(key);
    const std::lock_guard lock(stripe.mutex);
    Check(database.Delete(AsBytes(key)));
    stripe.entries.erase(key);
}

void WritePreferFresh(Database& database, StripedModel& model, std::shared_mutex& quiescence,
                      const MixedStressArguments& arguments, std::size_t worker,
                      std::uint64_t& fresh_counter, std::uint64_t fallback_slot,
                      const std::string& value) {
    const std::shared_lock active(quiescence);
    const std::uint64_t attempts = std::min<std::uint64_t>(arguments.keys, 64);
    for (std::uint64_t attempt = 0; attempt < attempts; ++attempt) {
        const std::uint64_t slot = (static_cast<std::uint64_t>(worker) +
                                    fresh_counter * static_cast<std::uint64_t>(arguments.threads)) %
                                   arguments.keys;
        ++fresh_counter;
        const std::string key = MixedKey(slot);
        StripedModel::Stripe& stripe = model.For(key);
        const std::lock_guard lock(stripe.mutex);
        if (!stripe.entries.contains(key)) {
            Check(database.Put(AsBytes(key), AsBytes(value)));
            stripe.entries[key] = value;
            return;
        }
    }
    const std::string key = MixedKey(fallback_slot);
    StripedModel::Stripe& stripe = model.For(key);
    const std::lock_guard lock(stripe.mutex);
    Check(database.Put(AsBytes(key), AsBytes(value)));
    stripe.entries[key] = value;
}

void ExecuteOperation(Database& database, StripedModel& model, std::shared_mutex& quiescence,
                      MixedCounters& counters, const MixedStressArguments& arguments,
                      std::size_t worker, std::size_t operation, std::uint64_t& fresh_counter) {
    const std::uint64_t selector = MixedRandom(arguments.seed, worker, operation, 0);
    const MixedOperationKind kind =
        SelectMixedOperation(static_cast<std::uint32_t>(selector % 100U));
    counters.operations[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t slot = MixedRandom(arguments.seed, worker, operation, 1) % arguments.keys;
    switch (kind) {
        case MixedOperationKind::Read:
            CheckPointRead(database, model, quiescence, MixedKey(slot));
            break;
        case MixedOperationKind::Prefix:
            CheckPrefix(database, model, quiescence, slot);
            break;
        case MixedOperationKind::Write: {
            const std::uint64_t token = MixedRandom(arguments.seed, worker, operation, 2);
            const std::string value = MixedValue(arguments.seed, worker, operation, token);
            if (MixedRandom(arguments.seed, worker, operation, 3) % 100U < 60U) {
                counters.fresh_write_plans.fetch_add(1, std::memory_order_relaxed);
                WritePreferFresh(database, model, quiescence, arguments, worker, fresh_counter,
                                 slot, value);
            } else {
                WriteValue(database, model, quiescence, MixedKey(slot), value);
            }
            break;
        }
        case MixedOperationKind::Delete:
            DeleteValue(database, model, quiescence, MixedKey(slot));
            break;
        case MixedOperationKind::Iterate:
            CheckIteration(database, model, quiescence, slot,
                           MixedRandom(arguments.seed, worker, operation, 4) % 2U != 0);
            break;
    }
}

void RethrowFirst(const std::vector<std::exception_ptr>& errors) {
    for (const std::exception_ptr& error : errors) {
        if (error != nullptr) {
            std::rethrow_exception(error);
        }
    }
}

void RunEpoch(Database& database, StripedModel& model, std::shared_mutex& quiescence,
              MixedCounters& counters, const MixedStressArguments& arguments, std::size_t epoch,
              std::size_t operation_start, std::size_t operation_count,
              std::vector<std::uint64_t>& fresh_counters) {
    std::atomic<bool> stop{false};
    StartGate start;
    std::vector<std::exception_ptr> errors(arguments.threads);
    std::vector<std::jthread> workers;
    workers.reserve(arguments.threads);
    try {
        for (std::size_t worker = 0; worker < arguments.threads; ++worker) {
            workers.emplace_back([&, worker] {
                if (!start.Wait()) {
                    return;
                }
                try {
                    for (std::size_t local = 0;
                         local < operation_count && !stop.load(std::memory_order_acquire);
                         ++local) {
                        const std::size_t operation = operation_start + local;
                        try {
                            ExecuteOperation(database, model, quiescence, counters, arguments,
                                             worker, operation, fresh_counters[worker]);
                        } catch (const std::exception& error) {
                            throw std::runtime_error("epoch=" + std::to_string(epoch) +
                                                     " worker=" + std::to_string(worker) +
                                                     " operation=" + std::to_string(operation) +
                                                     ": " + error.what());
                        }
                    }
                } catch (...) {
                    errors[worker] = std::current_exception();
                    stop.store(true, std::memory_order_release);
                }
            });
        }
    } catch (...) {
        stop.store(true, std::memory_order_release);
        start.Cancel();
        workers.clear();
        throw;
    }
    start.Open();
    workers.clear();
    RethrowFirst(errors);
}

std::map<std::string, std::string> VerifyExact(Database& database, StripedModel& model,
                                               std::shared_mutex& quiescence) {
    const std::unique_lock pause(quiescence);
    const std::map<std::string, std::string> expected = model.Snapshot();
    Iterator forward = Take(database.NewIterator());
    const auto actual_forward = ReadForward(forward);
    const std::vector<std::pair<std::string, std::string>> expected_forward(expected.begin(),
                                                                            expected.end());
    if (actual_forward != expected_forward) {
        throw std::runtime_error("mixed full forward state differs from model");
    }
    Iterator reverse = Take(database.NewIterator());
    const auto actual_reverse = ReadReverse(reverse);
    std::vector<std::pair<std::string, std::string>> expected_reverse(expected.rbegin(),
                                                                      expected.rend());
    if (actual_reverse != expected_reverse) {
        throw std::runtime_error("mixed full reverse state differs from model");
    }
    return expected;
}

std::size_t ParsePositiveSize(const std::filesystem::path& value, std::string_view option) {
    const std::uint64_t parsed = ParseStressUnsigned(value.string(), option);
    if (parsed == 0 || parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string(option) + " must fit size_t and be positive");
    }
    return static_cast<std::size_t>(parsed);
}

void WriteRun(const std::filesystem::path& output, const MixedStressArguments& arguments,
              const StressBuildIdentity& identity) {
    const std::string text =
        "format=" + std::string(RunFormat) + "\nrevision=" + identity.revision +
        "\ndirty=" + (identity.dirty ? "true" : "false") +
        "\nprofile=rocksdb-applicable-956c7042-v1" + "\nseed=" + std::to_string(arguments.seed) +
        "\nthreads=" + std::to_string(arguments.threads) +
        "\noperations_per_thread=" + std::to_string(arguments.operations_per_thread) +
        "\nkeys=" + std::to_string(arguments.keys) +
        "\nreopens=" + std::to_string(arguments.reopens) +
        "\ncompression=" + StressCompressionName(arguments.compression) +
        "\nread_percent=10\nprefix_percent=20\nwrite_percent=45"
        "\ndelete_percent=15\niterate_percent=10\nno_overwrite_percent=60\n";
    WriteStressText(output / "run.txt", text);
}

void WriteOperationSummary(const std::filesystem::path& output, const MixedStressSummary& summary) {
    const std::string text = "format=" + std::string(OperationFormat) +
                             "\nreads=" + std::to_string(summary.reads) +
                             "\nprefixes=" + std::to_string(summary.prefixes) +
                             "\nwrites=" + std::to_string(summary.writes) +
                             "\ndeletes=" + std::to_string(summary.deletes) +
                             "\niterations=" + std::to_string(summary.iterations) +
                             "\nfresh_write_plans=" + std::to_string(summary.fresh_write_plans) +
                             "\nexact_checks=" + std::to_string(summary.exact_checks) + "\n";
    WriteStressText(output / "operation-summary.txt", text);
}

void WriteFinalState(const std::filesystem::path& output, const MixedStressSummary& summary) {
    const std::string text = "format=" + std::string(StateFormat) +
                             "\nentries=" + std::to_string(summary.entries) +
                             "\nfingerprint=" + summary.fingerprint + "\n";
    WriteStressText(output / "final-state.txt", text);
}

void WriteCompletion(const std::filesystem::path& output, const MixedStressSummary& summary) {
    const std::string text =
        "format=" + std::string(ResultFormat) + "\nthreads=" + std::to_string(summary.threads) +
        "\noperations_per_thread=" + std::to_string(summary.operations_per_thread) +
        "\nreopens=" + std::to_string(summary.reopens) +
        "\nentries=" + std::to_string(summary.entries) + "\nfingerprint=" + summary.fingerprint +
        "\n";
    WriteStressText(output / "completion.txt", text);
}

void WriteFailure(const std::filesystem::path& output, std::string_view message) {
    WriteStressText(output / "failure.txt",
                    "format=" + std::string(ResultFormat) +
                        "\ncategory=runtime\nmessage=" + SanitizeStressMessage(message) + "\n");
}

}  // namespace

MixedStressArguments ParseMixedStressArguments(std::span<const std::filesystem::path> arguments) {
    MixedStressArguments result;
    if (arguments.size() == 1 && arguments.front() == std::filesystem::path("--help")) {
        result.help = true;
        return result;
    }
    bool have_database = false;
    bool have_output = false;
    bool have_seed = false;
    bool have_threads = false;
    bool have_operations = false;
    bool have_keys = false;
    bool have_reopens = false;
    bool have_compression = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::filesystem::path& option = arguments[index];
        if (option == std::filesystem::path("--help")) {
            throw std::invalid_argument("--help cannot be combined with mixed stress options");
        }
        if (index + 1 >= arguments.size()) {
            throw std::invalid_argument("mixed stress option requires a value");
        }
        const std::filesystem::path& value = arguments[++index];
        if (option == std::filesystem::path("--db")) {
            if (have_database || value.empty()) {
                throw std::invalid_argument("--db must be provided once with a nonempty path");
            }
            result.database = value;
            have_database = true;
        } else if (option == std::filesystem::path("--output")) {
            if (have_output || value.empty()) {
                throw std::invalid_argument("--output must be provided once with a nonempty path");
            }
            result.output = value;
            have_output = true;
        } else if (option == std::filesystem::path("--seed")) {
            if (have_seed) {
                throw std::invalid_argument("--seed was provided more than once");
            }
            result.seed = ParseStressUnsigned(value.string(), "--seed");
            have_seed = true;
        } else if (option == std::filesystem::path("--threads")) {
            if (have_threads) {
                throw std::invalid_argument("--threads was provided more than once");
            }
            result.threads = ParsePositiveSize(value, "--threads");
            have_threads = true;
        } else if (option == std::filesystem::path("--operations-per-thread")) {
            if (have_operations) {
                throw std::invalid_argument("--operations-per-thread was provided more than once");
            }
            result.operations_per_thread = ParsePositiveSize(value, "--operations-per-thread");
            have_operations = true;
        } else if (option == std::filesystem::path("--keys")) {
            if (have_keys) {
                throw std::invalid_argument("--keys was provided more than once");
            }
            result.keys = ParseStressUnsigned(value.string(), "--keys");
            if (result.keys == 0) {
                throw std::invalid_argument("--keys must be positive");
            }
            have_keys = true;
        } else if (option == std::filesystem::path("--reopens")) {
            if (have_reopens) {
                throw std::invalid_argument("--reopens was provided more than once");
            }
            const std::uint64_t reopens = ParseStressUnsigned(value.string(), "--reopens");
            if (reopens > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument("--reopens must fit size_t");
            }
            result.reopens = static_cast<std::size_t>(reopens);
            have_reopens = true;
        } else if (option == std::filesystem::path("--compression")) {
            if (have_compression) {
                throw std::invalid_argument("--compression was provided more than once");
            }
            result.compression = ParseStressCompression(value.string());
            have_compression = true;
        } else {
            throw std::invalid_argument("unknown mixed stress option");
        }
    }
    if (!have_database || !have_output) {
        throw std::invalid_argument("--db and --output are required");
    }
    static_cast<void>(MixedEpochOperationCounts(result.operations_per_thread, result.reopens));
    return result;
}

MixedStressSummary RunMixedStress(const MixedStressArguments& arguments,
                                  const StressBuildIdentity& identity,
                                  MixedStressObserver* observer) {
    static_cast<void>(
        MixedEpochOperationCounts(arguments.operations_per_thread, arguments.reopens));
    if (arguments.threads == 0 || arguments.keys == 0) {
        throw std::invalid_argument("mixed threads and keys must be positive");
    }
    const PreparedStressPaths paths = PrepareStressPaths(arguments.database, arguments.output);
    WriteRun(paths.output, arguments, identity);
    CheckedOutput epochs(paths.output / "epoch-summary.tsv");
    epochs.Write("format=" + std::string(EpochFormat) + "\n");
    epochs.Write("epoch\toperations_per_thread\tentries\texact_checks\treopened\n");
    try {
        Options options = StressDatabaseOptions(arguments.compression);
        std::optional<Database> database(Take(Database::Open(options, paths.database)));
        options.error_if_exists = false;
        StripedModel model;
        std::shared_mutex quiescence;
        MixedCounters counters;
        std::vector<std::uint64_t> fresh_counters(arguments.threads, 0);
        const std::vector<std::size_t> epoch_counts =
            MixedEpochOperationCounts(arguments.operations_per_thread, arguments.reopens);
        std::size_t operation_start = 0;
        std::size_t exact_checks = 0;
        std::map<std::string, std::string> final_state;
        for (std::size_t epoch = 0; epoch < epoch_counts.size(); ++epoch) {
            if (observer != nullptr) {
                observer->BeforeEpoch(epoch);
            }
            RunEpoch(*database, model, quiescence, counters, arguments, epoch, operation_start,
                     epoch_counts[epoch], fresh_counters);
            operation_start += epoch_counts[epoch];
            final_state = VerifyExact(*database, model, quiescence);
            ++exact_checks;
            bool reopened = false;
            if (epoch + 1U != epoch_counts.size()) {
                database.reset();
                database.emplace(Take(Database::Open(options, paths.database)));
                final_state = VerifyExact(*database, model, quiescence);
                ++exact_checks;
                reopened = true;
            }
            epochs.Write(std::to_string(epoch) + "\t" + std::to_string(epoch_counts[epoch]) + "\t" +
                         std::to_string(final_state.size()) + "\t" + std::to_string(exact_checks) +
                         "\t" + (reopened ? "true\n" : "false\n"));
        }

        const MixedStressSummary summary{
            arguments.threads,
            arguments.operations_per_thread,
            arguments.reopens,
            counters.operations[static_cast<std::size_t>(MixedOperationKind::Read)].load(
                std::memory_order_relaxed),
            counters.operations[static_cast<std::size_t>(MixedOperationKind::Prefix)].load(
                std::memory_order_relaxed),
            counters.operations[static_cast<std::size_t>(MixedOperationKind::Write)].load(
                std::memory_order_relaxed),
            counters.operations[static_cast<std::size_t>(MixedOperationKind::Delete)].load(
                std::memory_order_relaxed),
            counters.operations[static_cast<std::size_t>(MixedOperationKind::Iterate)].load(
                std::memory_order_relaxed),
            counters.fresh_write_plans.load(std::memory_order_relaxed),
            exact_checks,
            final_state.size(),
            StableStressFingerprint(final_state),
        };
        WriteOperationSummary(paths.output, summary);
        WriteFinalState(paths.output, summary);
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
            WriteFailure(paths.output, "unknown mixed stress failure");
        } catch (const std::exception& artifact_error) {
            throw std::runtime_error(
                "unknown mixed stress failure; cannot write failure artifact: " +
                std::string(artifact_error.what()));
        }
        throw;
    }
}

std::string MixedStressUsage() {
    return "usage: modern_leveldb_mixed_stress --db DISPOSABLE_DATABASE_DIRECTORY "
           "--output NEW_ARTIFACT_DIRECTORY [--seed N] [--threads N] "
           "[--operations-per-thread N] [--keys N] [--reopens N] "
           "[--compression none|snappy|zstd]\n"
           "Defaults: threads=32 operations-per-thread=1200000 keys=1048576 "
           "reopens=10 compression=snappy. The pinned operation mix is "
           "10/20/45/15/10 for read/prefix/write/delete/iterate. Both paths "
           "must be absent, distinct, non-nested, and used only for disposable "
           "local stress data.\n";
}

}  // namespace modern_leveldb::stress
