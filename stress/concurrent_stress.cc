#include "stress/concurrent_stress.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "modern_leveldb/db.h"
#include "modern_leveldb/write_batch.h"
#include "stress/linearizability.h"

namespace modern_leveldb::stress {
namespace {

constexpr std::string_view RunFormat = "modern-leveldb-concurrent-stress-run-v1";
constexpr std::string_view HistoryFormat = "modern-leveldb-linearizability-history-v1";
constexpr std::string_view SnapshotFormat = "modern-leveldb-snapshot-summary-v1";
constexpr std::string_view StateFormat = "modern-leveldb-concurrent-state-v1";
constexpr std::string_view ResultFormat = "modern-leveldb-concurrent-result-v1";
constexpr std::string_view HotKey = "hot-key";

enum class PlannedKind : std::uint8_t {
    Store,
    Delete,
    Read,
    Batch,
};

struct PlannedOperation {
    PlannedKind kind = PlannedKind::Read;
    std::string value;
    unsigned yields_before = 0;
    unsigned yields_after = 0;
    bool sync = false;
};

struct LinearizabilityPhase {
    std::optional<std::string> final_value;
    std::size_t worker_operations = 0;
};

struct SnapshotPhase {
    std::size_t batch_writes = 0;
    std::size_t snapshot_checks = 0;
};

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

std::map<std::string, std::string> ConsumeIterator(Iterator& iterator) {
    Check(iterator.SeekToFirst());
    std::map<std::string, std::string> result;
    while (iterator.valid()) {
        const auto [entry, inserted] =
            result.emplace(AsStringView(iterator.key()), AsStringView(iterator.value()));
        static_cast<void>(entry);
        if (!inserted) {
            throw std::runtime_error("iterator returned a duplicate key");
        }
        Check(iterator.Next());
    }
    return result;
}

std::map<std::string, std::string> Scan(Database& database) {
    Iterator iterator = Take(database.NewIterator());
    return ConsumeIterator(iterator);
}

std::string HotValue(std::size_t round, std::size_t worker, std::uint64_t token) {
    return "hot/" + std::to_string(round) + "/" + std::to_string(worker) + "/" +
           std::to_string(token);
}

std::vector<PlannedOperation> PlanRound(std::mt19937_64& random, std::size_t round,
                                        std::size_t threads) {
    std::vector<PlannedOperation> result;
    result.reserve(threads);
    for (std::size_t worker = 0; worker != threads; ++worker) {
        PlannedOperation operation;
        operation.kind = static_cast<PlannedKind>(random() % 4);
        operation.value = HotValue(round, worker, random());
        operation.yields_before = static_cast<unsigned>(random() % 4);
        operation.yields_after = static_cast<unsigned>(random() % 4);
        operation.sync = (round * threads + worker) % 17 == 0;
        result.push_back(std::move(operation));
    }
    return result;
}

void Yield(unsigned count) {
    for (unsigned index = 0; index != count; ++index) {
        std::this_thread::yield();
    }
}

RegisterOperation ExecutePlanned(Database& database, const PlannedOperation& plan, std::size_t id,
                                 std::atomic<std::uint64_t>& tick) {
    RegisterOperation operation;
    operation.id = id;
    operation.invoke = tick.fetch_add(1, std::memory_order_relaxed);
    Yield(plan.yields_before);
    switch (plan.kind) {
        case PlannedKind::Store:
            operation.kind = RegisterOperationKind::Store;
            operation.value = plan.value;
            Check(database.Put(AsBytes(HotKey), AsBytes(plan.value), {.sync = plan.sync}));
            break;
        case PlannedKind::Delete:
            operation.kind = RegisterOperationKind::Delete;
            Check(database.Delete(AsBytes(HotKey), {.sync = plan.sync}));
            break;
        case PlannedKind::Read:
            operation.kind = RegisterOperationKind::Read;
            operation.observed = ReadValue(database, HotKey);
            break;
        case PlannedKind::Batch: {
            operation.kind = RegisterOperationKind::Store;
            operation.value = plan.value;
            WriteBatch batch;
            Check(batch.Delete(AsBytes(HotKey)));
            Check(batch.Put(AsBytes(HotKey), AsBytes(plan.value)));
            Check(database.Write(batch, {.sync = plan.sync}));
            break;
        }
    }
    Yield(plan.yields_after);
    operation.complete = tick.fetch_add(1, std::memory_order_relaxed);
    return operation;
}

std::string OptionalHex(const std::optional<std::string>& value) {
    return value.has_value() ? StressHex(*value) : "missing";
}

std::string RegisterKindName(RegisterOperationKind kind) {
    switch (kind) {
        case RegisterOperationKind::Store:
            return "store";
        case RegisterOperationKind::Delete:
            return "delete";
        case RegisterOperationKind::Read:
            return "read";
    }
    throw std::invalid_argument("unknown register operation kind");
}

std::string PlannedKindName(PlannedKind kind) {
    switch (kind) {
        case PlannedKind::Store:
            return "put";
        case PlannedKind::Delete:
            return "delete";
        case PlannedKind::Read:
            return "get";
        case PlannedKind::Batch:
            return "batch";
    }
    throw std::invalid_argument("unknown planned operation kind");
}

std::string DescribeHistory(std::size_t round, const std::optional<std::string>& initial,
                            std::span<const RegisterOperation> operations,
                            std::span<const PlannedOperation> plans,
                            const std::optional<LinearizationWitness>& witness) {
    std::string result =
        "round=" + std::to_string(round) + "\tinitial=" + OptionalHex(initial) + "\n";
    for (const RegisterOperation& operation : operations) {
        result += "op=" + std::to_string(operation.id) +
                  "\tkind=" + RegisterKindName(operation.kind) +
                  "\tinvoke=" + std::to_string(operation.invoke) +
                  "\tcomplete=" + std::to_string(operation.complete);
        if (operation.id < plans.size()) {
            const PlannedOperation& plan = plans[operation.id];
            result += "\tsource=" + PlannedKindName(plan.kind) +
                      "\tsync=" + std::to_string(plan.sync) +
                      "\tyields_before=" + std::to_string(plan.yields_before) +
                      "\tyields_after=" + std::to_string(plan.yields_after);
        } else {
            result += "\tsource=final-read";
        }
        if (operation.value.has_value()) {
            result += "\tvalue=" + StressHex(*operation.value);
        }
        if (operation.kind == RegisterOperationKind::Read) {
            result += "\tobserved=" + OptionalHex(operation.observed);
        }
        result.push_back('\n');
    }
    if (!witness.has_value()) {
        result += "witness=invalid\n";
        return result;
    }
    result += "witness=";
    for (std::size_t index = 0; index != witness->operation_ids.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        result += std::to_string(witness->operation_ids[index]);
    }
    result += "\tfinal=" + OptionalHex(witness->final_value) + "\n";
    return result;
}

void RethrowFirst(const std::vector<std::exception_ptr>& errors) {
    for (const std::exception_ptr& error : errors) {
        if (error != nullptr) {
            std::rethrow_exception(error);
        }
    }
}

LinearizabilityPhase RunLinearizabilityPhase(Database& database,
                                             const ConcurrentStressArguments& arguments,
                                             ConcurrentStressObserver* observer,
                                             CheckedOutput& history_log) {
    std::mt19937_64 random(arguments.seed);
    std::atomic<std::uint64_t> tick{1};
    std::optional<std::string> state;
    for (std::size_t round = 0; round != arguments.rounds; ++round) {
        if (observer != nullptr) {
            observer->BeforeLinearizabilityRound(round);
        }
        const std::vector<PlannedOperation> plans = PlanRound(random, round, arguments.threads);
        std::vector<RegisterOperation> history(arguments.threads);
        std::vector<std::exception_ptr> errors(arguments.threads);
        StartGate start;
        std::vector<std::jthread> workers;
        workers.reserve(arguments.threads);
        try {
            for (std::size_t worker = 0; worker != arguments.threads; ++worker) {
                workers.emplace_back([&, worker] {
                    if (!start.Wait()) {
                        return;
                    }
                    try {
                        history[worker] = ExecutePlanned(database, plans[worker], worker, tick);
                    } catch (...) {
                        errors[worker] = std::current_exception();
                    }
                });
            }
        } catch (...) {
            start.Cancel();
            workers.clear();
            throw;
        }
        start.Open();
        workers.clear();
        RethrowFirst(errors);

        RegisterOperation final_read;
        final_read.id = arguments.threads;
        final_read.kind = RegisterOperationKind::Read;
        final_read.invoke = tick.fetch_add(1, std::memory_order_relaxed);
        final_read.observed = ReadValue(database, HotKey);
        final_read.complete = tick.fetch_add(1, std::memory_order_relaxed);
        history.push_back(std::move(final_read));

        const auto witness = FindRegisterLinearization(state, history);
        history_log.Write(DescribeHistory(round, state, history, plans, witness));
        if (!witness.has_value()) {
            throw std::runtime_error("concurrent register history is not linearizable");
        }
        state = witness->final_value;
    }
    return {state, arguments.rounds * arguments.threads};
}

std::string PartitionKey(std::size_t writer, char suffix) {
    return "partition/" + std::to_string(writer) + "/" + suffix;
}

std::string GenerationValue(std::size_t writer, std::size_t generation) {
    return "generation/" + std::to_string(writer) + "/" + std::to_string(generation);
}

std::size_t ParseGeneration(std::string_view value, std::size_t writer) {
    const std::string prefix = "generation/" + std::to_string(writer) + "/";
    if (!value.starts_with(prefix)) {
        throw std::runtime_error("snapshot value has the wrong writer identity");
    }
    const std::string_view generation_text = value.substr(prefix.size());
    std::uint64_t generation = 0;
    const auto parsed = std::from_chars(
        generation_text.data(), generation_text.data() + generation_text.size(), generation);
    if (generation_text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != generation_text.data() + generation_text.size() ||
        generation > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("snapshot value has an invalid generation");
    }
    return static_cast<std::size_t>(generation);
}

void ValidatePartitionPair(const std::optional<std::string>& left,
                           const std::optional<std::string>& right, std::size_t writer,
                           std::size_t issued) {
    const std::string context = "writer " + std::to_string(writer) + ": ";
    if (left.has_value() != right.has_value()) {
        throw std::runtime_error(context + "snapshot exposed a partial atomic batch");
    }
    if (!left.has_value()) {
        return;
    }
    if (*left != *right) {
        throw std::runtime_error(context + "snapshot exposed different batch generations");
    }
    const std::size_t generation = ParseGeneration(*left, writer);
    if (generation == 0 || generation > issued) {
        throw std::runtime_error(context + "snapshot exposed generation " +
                                 std::to_string(generation) + " above issued " +
                                 std::to_string(issued));
    }
}

void CheckSnapshotView(Database& database, std::size_t writer_count,
                       const std::unique_ptr<std::atomic<std::size_t>[]>& issued) {
    std::optional<Snapshot> snapshot(Take(database.GetSnapshot()));
    ReadOptions options{.snapshot = &*snapshot};
    for (std::size_t writer = 0; writer != writer_count; ++writer) {
        const auto left = ReadValue(database, PartitionKey(writer, 'a'), options);
        const auto right = ReadValue(database, PartitionKey(writer, 'b'), options);
        ValidatePartitionPair(left, right, writer, issued[writer].load(std::memory_order_acquire));
    }
    Iterator iterator = Take(database.NewIterator(options));
    snapshot.reset();
    const std::map<std::string, std::string> entries = ConsumeIterator(iterator);
    for (std::size_t writer = 0; writer != writer_count; ++writer) {
        std::optional<std::string> left;
        std::optional<std::string> right;
        if (const auto entry = entries.find(PartitionKey(writer, 'a')); entry != entries.end()) {
            left = entry->second;
        }
        if (const auto entry = entries.find(PartitionKey(writer, 'b')); entry != entries.end()) {
            right = entry->second;
        }
        ValidatePartitionPair(left, right, writer, issued[writer].load(std::memory_order_acquire));
    }
}

SnapshotPhase RunSnapshotPhase(Database& database, const ConcurrentStressArguments& arguments) {
    const std::size_t writer_count = std::max<std::size_t>(1, arguments.threads / 2);
    const std::size_t reader_count = arguments.threads - writer_count;
    auto issued = std::make_unique<std::atomic<std::size_t>[]>(writer_count);
    auto completed = std::make_unique<std::atomic<std::size_t>[]>(writer_count);
    for (std::size_t writer = 0; writer != writer_count; ++writer) {
        issued[writer].store(0, std::memory_order_relaxed);
        completed[writer].store(0, std::memory_order_relaxed);
    }
    std::atomic<std::size_t> completed_writers{0};
    std::atomic<std::size_t> batch_writes{0};
    std::atomic<std::size_t> snapshot_checks{0};
    std::atomic<bool> stop{false};
    StartGate start;
    std::vector<std::exception_ptr> errors(arguments.threads);
    std::vector<std::jthread> threads;
    threads.reserve(arguments.threads);

    try {
        for (std::size_t writer = 0; writer != writer_count; ++writer) {
            threads.emplace_back([&, writer] {
                if (!start.Wait()) {
                    return;
                }
                try {
                    for (std::size_t generation = 1;
                         generation <= arguments.rounds && !stop.load(std::memory_order_acquire);
                         ++generation) {
                        const std::string value = GenerationValue(writer, generation);
                        issued[writer].store(generation, std::memory_order_release);
                        WriteBatch batch;
                        Check(batch.Put(AsBytes(PartitionKey(writer, 'a')), AsBytes(value)));
                        Check(batch.Put(AsBytes(PartitionKey(writer, 'b')), AsBytes(value)));
                        Check(database.Write(batch, {.sync = generation % 17 == 0}));
                        completed[writer].store(generation, std::memory_order_release);
                        batch_writes.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                    }
                    completed_writers.fetch_add(1, std::memory_order_release);
                } catch (...) {
                    errors[writer] = std::current_exception();
                    stop.store(true, std::memory_order_release);
                }
            });
        }
        for (std::size_t reader = 0; reader != reader_count; ++reader) {
            const std::size_t slot = writer_count + reader;
            threads.emplace_back([&, slot] {
                if (!start.Wait()) {
                    return;
                }
                try {
                    do {
                        CheckSnapshotView(database, writer_count, issued);
                        snapshot_checks.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                    } while (!stop.load(std::memory_order_acquire) &&
                             completed_writers.load(std::memory_order_acquire) != writer_count);
                } catch (...) {
                    errors[slot] = std::current_exception();
                    stop.store(true, std::memory_order_release);
                }
            });
        }
    } catch (...) {
        stop.store(true, std::memory_order_release);
        start.Cancel();
        threads.clear();
        throw;
    }
    start.Open();
    threads.clear();
    RethrowFirst(errors);
    for (std::size_t writer = 0; writer != writer_count; ++writer) {
        if (completed[writer].load(std::memory_order_acquire) != arguments.rounds) {
            throw std::runtime_error("concurrent writer did not complete every generation");
        }
    }
    return {
        batch_writes.load(std::memory_order_relaxed),
        snapshot_checks.load(std::memory_order_relaxed),
    };
}

std::map<std::string, std::string> ExpectedFinalState(const ConcurrentStressArguments& arguments,
                                                      const std::optional<std::string>& hot_value) {
    std::map<std::string, std::string> expected;
    if (hot_value.has_value()) {
        expected.emplace(HotKey, *hot_value);
    }
    const std::size_t writer_count = std::max<std::size_t>(1, arguments.threads / 2);
    for (std::size_t writer = 0; writer != writer_count; ++writer) {
        const std::string value = GenerationValue(writer, arguments.rounds);
        expected.emplace(PartitionKey(writer, 'a'), value);
        expected.emplace(PartitionKey(writer, 'b'), value);
    }
    return expected;
}

void WriteRun(const std::filesystem::path& output, const ConcurrentStressArguments& arguments,
              const StressBuildIdentity& identity) {
    const std::string text = "format=" + std::string(RunFormat) +
                             "\nrevision=" + identity.revision +
                             "\ndirty=" + (identity.dirty ? "true" : "false") +
                             "\nseed=" + std::to_string(arguments.seed) +
                             "\nmode=" + ConcurrentStressModeName(arguments.mode) +
                             "\nthreads=" + std::to_string(arguments.threads) +
                             "\nrounds=" + std::to_string(arguments.rounds) +
                             "\ncompression=" + StressCompressionName(arguments.compression) +
                             "\nwrite_buffer_size=" + std::to_string(StressWriteBufferSize) +
                             "\nbloom_bits_per_key=" + std::to_string(StressBloomBitsPerKey) + "\n";
    WriteStressText(output / "run.txt", text);
}

void WriteSnapshotSummary(const std::filesystem::path& output,
                          const ConcurrentStressArguments& arguments, const SnapshotPhase& phase) {
    const std::string text = "format=" + std::string(SnapshotFormat) + "\nwriters=" +
                             std::to_string(std::max<std::size_t>(1, arguments.threads / 2)) +
                             "\nbatch_writes=" + std::to_string(phase.batch_writes) +
                             "\nsnapshot_checks=" + std::to_string(phase.snapshot_checks) + "\n";
    WriteStressText(output / "snapshot-summary.txt", text);
}

void WriteFinalState(const std::filesystem::path& output,
                     const std::map<std::string, std::string>& entries) {
    CheckedOutput state(output / "final-state.txt");
    state.Write("format=" + std::string(StateFormat) + "\n");
    for (const auto& [key, value] : entries) {
        state.Write(StressHex(key) + "\t" + StressHex(value) + "\n");
    }
}

void WriteCompletion(const std::filesystem::path& output, const ConcurrentStressSummary& summary) {
    const std::string text =
        "format=" + std::string(ResultFormat) + "\nrounds=" + std::to_string(summary.rounds) +
        "\nthreads=" + std::to_string(summary.threads) +
        "\nmode=" + ConcurrentStressModeName(summary.mode) +
        "\nlinearized_worker_operations=" + std::to_string(summary.linearized_worker_operations) +
        "\nbatch_writes=" + std::to_string(summary.batch_writes) +
        "\nsnapshot_checks=" + std::to_string(summary.snapshot_checks) +
        "\nentries=" + std::to_string(summary.entries) + "\n";
    WriteStressText(output / "completion.txt", text);
}

void WriteFailure(const std::filesystem::path& output, std::string_view message) {
    const std::string text = "format=" + std::string(ResultFormat) +
                             "\ncategory=runtime\nmessage=" + SanitizeStressMessage(message) + "\n";
    WriteStressText(output / "failure.txt", text);
}

}  // namespace

ConcurrentStressArguments ParseConcurrentStressArguments(
    std::span<const std::filesystem::path> arguments) {
    ConcurrentStressArguments result;
    if (arguments.size() == 1 && arguments.front() == std::filesystem::path("--help")) {
        result.help = true;
        return result;
    }
    bool have_database = false;
    bool have_output = false;
    bool have_seed = false;
    bool have_mode = false;
    bool have_threads = false;
    bool have_rounds = false;
    bool have_compression = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::filesystem::path& option = arguments[index];
        if (option == std::filesystem::path("--help")) {
            throw std::invalid_argument("--help cannot be combined with concurrent options");
        }
        if (index + 1 >= arguments.size()) {
            throw std::invalid_argument("concurrent stress option requires a value");
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
        } else if (option == std::filesystem::path("--mode")) {
            if (have_mode) {
                throw std::invalid_argument("--mode was provided more than once");
            }
            if (value == std::filesystem::path("checked")) {
                result.mode = ConcurrentStressMode::Checked;
            } else if (value == std::filesystem::path("fanout")) {
                result.mode = ConcurrentStressMode::Fanout;
            } else {
                throw std::invalid_argument("unknown concurrent stress mode");
            }
            have_mode = true;
        } else if (option == std::filesystem::path("--threads")) {
            if (have_threads) {
                throw std::invalid_argument("--threads was provided more than once");
            }
            const std::uint64_t threads = ParseStressUnsigned(value.string(), "--threads");
            if (threads > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument("--threads must fit size_t");
            }
            result.threads = static_cast<std::size_t>(threads);
            have_threads = true;
        } else if (option == std::filesystem::path("--rounds")) {
            if (have_rounds) {
                throw std::invalid_argument("--rounds was provided more than once");
            }
            const std::uint64_t rounds = ParseStressUnsigned(value.string(), "--rounds");
            if (rounds == 0 || rounds > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument("--rounds must fit size_t and be positive");
            }
            result.rounds = static_cast<std::size_t>(rounds);
            have_rounds = true;
        } else if (option == std::filesystem::path("--compression")) {
            if (have_compression) {
                throw std::invalid_argument("--compression was provided more than once");
            }
            result.compression = ParseStressCompression(value.string());
            have_compression = true;
        } else {
            throw std::invalid_argument("unknown concurrent stress option");
        }
    }
    if (!have_database || !have_output) {
        throw std::invalid_argument("--db and --output are required");
    }
    const std::size_t maximum_threads = result.mode == ConcurrentStressMode::Checked ? 8U : 256U;
    if (result.threads < 2 || result.threads > maximum_threads) {
        throw std::invalid_argument(result.mode == ConcurrentStressMode::Checked
                                        ? "--threads must be in 2..8 for checked mode"
                                        : "--threads must be in 2..256 for fanout mode");
    }
    return result;
}

std::string ConcurrentStressModeName(ConcurrentStressMode mode) {
    switch (mode) {
        case ConcurrentStressMode::Checked:
            return "checked";
        case ConcurrentStressMode::Fanout:
            return "fanout";
    }
    throw std::invalid_argument("unknown concurrent stress mode");
}

ConcurrentStressSummary RunConcurrentStress(const ConcurrentStressArguments& arguments,
                                            const StressBuildIdentity& identity,
                                            ConcurrentStressObserver* observer) {
    const PreparedStressPaths paths = PrepareStressPaths(arguments.database, arguments.output);
    WriteRun(paths.output, arguments, identity);
    CheckedOutput history_log(paths.output / "linearizability.log");
    history_log.Write("format=" + std::string(HistoryFormat) + "\n");
    try {
        Options options = StressDatabaseOptions(arguments.compression);
        std::optional<Database> database(Take(Database::Open(options, paths.database)));
        LinearizabilityPhase linear;
        if (arguments.mode == ConcurrentStressMode::Checked) {
            linear = RunLinearizabilityPhase(*database, arguments, observer, history_log);
        }
        const SnapshotPhase snapshots = RunSnapshotPhase(*database, arguments);
        WriteSnapshotSummary(paths.output, arguments, snapshots);

        const std::map<std::string, std::string> expected =
            ExpectedFinalState(arguments, linear.final_value);
        std::map<std::string, std::string> retained;
        {
            std::optional<Snapshot> snapshot(Take(database->GetSnapshot()));
            ReadOptions read_options{.snapshot = &*snapshot};
            Iterator iterator = Take(database->NewIterator(read_options));
            snapshot.reset();
            database.reset();
            retained = ConsumeIterator(iterator);
        }
        if (retained != expected) {
            throw std::runtime_error(
                "iterator retained the wrong state after Database destruction");
        }

        options.error_if_exists = false;
        database.emplace(Take(Database::Open(options, paths.database)));
        const std::map<std::string, std::string> actual = Scan(*database);
        if (actual != expected) {
            throw std::runtime_error("final concurrent state differs after reopen");
        }
        const ConcurrentStressSummary summary{
            arguments.rounds,       arguments.threads,
            arguments.mode,         linear.worker_operations,
            snapshots.batch_writes, snapshots.snapshot_checks,
            actual.size(),
        };
        WriteFinalState(paths.output, actual);
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
            WriteFailure(paths.output, "unknown concurrent stress failure");
        } catch (const std::exception& artifact_error) {
            throw std::runtime_error(
                "unknown concurrent stress failure; cannot write failure artifact: " +
                std::string(artifact_error.what()));
        }
        throw;
    }
}

std::string ConcurrentStressUsage() {
    return "usage: modern_leveldb_concurrent_stress --db DISPOSABLE_DATABASE_DIRECTORY "
           "--output NEW_ARTIFACT_DIRECTORY [--seed N] [--mode checked|fanout] "
           "[--threads N] [--rounds N] [--compression none|snappy|zstd]\n"
           "Checked mode bounds threads to 2..8 and includes bounded linearizability. "
           "Fanout mode bounds threads to 2..256 and runs the scalable batch/snapshot oracle. "
           "Both paths must be absent, distinct, non-nested, and used only for disposable "
           "local stress data.\n";
}

}  // namespace modern_leveldb::stress
