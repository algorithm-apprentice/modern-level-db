#include "stress/option_fault_stress.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "engine/database.h"
#include "format/write_batch.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "support/manual_clock.h"
#include "support/manual_executor.h"
#include "support/memory_file_system.h"

namespace modern_leveldb::stress {
namespace {

using test_support::ManualClock;
using test_support::ManualExecutor;
using test_support::MemoryFileSystem;

constexpr std::string_view FirstKey = "fault/a";
constexpr std::string_view SecondKey = "fault/b";

struct BatchValues {
    std::string first;
    std::string second;
};

class FailOnce final {
public:
    FailOnce(std::function<bool(std::string_view)> matches, Error error)
        : matches_(std::move(matches)), error_(std::move(error)) {}

    Status operator()(std::string_view operation) {
        const std::lock_guard lock(mutex_);
        if (!injected_ && matches_(operation)) {
            injected_ = true;
            operation_ = operation;
            return std::unexpected(error_);
        }
        return {};
    }

    std::string operation() const {
        const std::lock_guard lock(mutex_);
        if (!injected_) {
            throw std::runtime_error("fault scenario did not reach its selected operation");
        }
        return operation_;
    }

private:
    mutable std::mutex mutex_;
    std::function<bool(std::string_view)> matches_;
    Error error_;
    bool injected_ = false;
    std::string operation_;
};

class FaultHarness final {
public:
    FaultHarness() {
        options_.file_system = &file_system_;
        options_.executor = &executor_;
        options_.clock = &clock_;
        options_.create_if_missing = true;
        options_.write_buffer_size = 1;
    }

    std::unique_ptr<DatabaseEngine> Open() {
        Result<std::unique_ptr<DatabaseEngine>> opened =
            DatabaseEngine::Open(options_, directory_);
        if (!opened.has_value()) {
            throw std::runtime_error("cannot open fault database: " +
                                     opened.error().ToString());
        }
        return std::move(*opened);
    }

    MemoryFileSystem& file_system() noexcept { return file_system_; }
    ManualExecutor& executor() noexcept { return executor_; }

private:
    MemoryFileSystem file_system_;
    ManualExecutor executor_;
    ManualClock clock_;
    DatabaseEngineOptions options_;
    const std::filesystem::path directory_ = "db";
};

BatchValues SmallValues(std::string_view prefix) {
    return {
        std::string(prefix) + "-a",
        std::string(prefix) + "-b",
    };
}

BatchValues LargeOldValues() {
    return {
        std::string(66'000, 'a'),
        std::string(66'000, 'b'),
    };
}

Status WritePair(DatabaseEngine& database, const BatchValues& values, bool sync = false) {
    EncodedWriteBatch batch;
    batch.Put(AsBytes(FirstKey), AsBytes(values.first));
    batch.Put(AsBytes(SecondKey), AsBytes(values.second));
    return database.Write(batch, sync);
}

std::optional<std::string> ReadValue(DatabaseEngine& database, std::string_view key) {
    std::vector<std::byte> value;
    Result<bool> found = database.Get(AsBytes(key), value);
    if (!found.has_value()) {
        throw std::runtime_error("fault recovery read failed: " +
                                 found.error().ToString());
    }
    if (!*found) {
        return std::nullopt;
    }
    return std::string(AsStringView(value));
}

void RequireSuccess(const Status& status, std::string_view operation) {
    if (!status.has_value()) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 status.error().ToString());
    }
}

Status Flush(FaultHarness& harness, DatabaseEngine& database) {
    std::optional<Status> result;
    std::thread flusher([&] { result = database.FlushMemTable(); });
    harness.executor().WaitForTask();
    harness.executor().RunAll();
    flusher.join();
    if (!result.has_value()) {
        throw std::runtime_error("flush thread did not publish a result");
    }
    return std::move(*result);
}

RecoveredBatchState RecoverBatch(FaultHarness& harness,
                                 std::unique_ptr<DatabaseEngine>& database,
                                 const BatchValues& old_values,
                                 const BatchValues& new_values) {
    database.reset();
    database = harness.Open();
    harness.executor().RunAll();
    RequireSuccess(database->WaitForBackgroundWork(), "recovery background work");
    const std::optional<std::string> first = ReadValue(*database, FirstKey);
    const std::optional<std::string> second = ReadValue(*database, SecondKey);
    if (first == old_values.first && second == old_values.second) {
        return RecoveredBatchState::Old;
    }
    if (first == new_values.first && second == new_values.second) {
        return RecoveredBatchState::New;
    }
    throw std::runtime_error("fault recovery produced a partial or unexpected batch state");
}

FaultScenarioSummary ReadScenario() {
    FaultHarness harness;
    std::unique_ptr<DatabaseEngine> database = harness.Open();
    const BatchValues values = SmallValues("table");
    RequireSuccess(WritePair(*database, values), "baseline write");
    RequireSuccess(Flush(harness, *database), "baseline flush");
    database.reset();
    database = harness.Open();

    const std::string message = "injected read failure";
    FailOnce failure(
        [](std::string_view operation) {
            return operation.starts_with("read ") && operation.ends_with(".ldb");
        },
        Error::Io(message));
    harness.file_system().SetOperationHook(std::ref(failure));
    std::vector<std::byte> value;
    const Result<bool> failed = database->Get(AsBytes(FirstKey), value);
    harness.file_system().SetOperationHook({});
    if (failed.has_value()) {
        throw std::runtime_error("read fault unexpectedly succeeded");
    }
    const bool retry_succeeded =
        ReadValue(*database, FirstKey) == values.first &&
        ReadValue(*database, SecondKey) == values.second;
    const Result<DatabaseEngineState> state = database->GetState();
    if (!state.has_value()) {
        throw std::runtime_error("cannot inspect read-fault state: " +
                                 state.error().ToString());
    }
    return {
        FaultScenario::Read,
        FaultScenarioName(FaultScenario::Read),
        failure.operation(),
        failed.error().code(),
        std::string(failed.error().message()),
        state->sticky_error.has_value(),
        false,
        retry_succeeded,
        std::nullopt,
    };
}

bool IsLogOperation(std::string_view operation, std::string_view verb) {
    return operation.starts_with(verb) && operation.ends_with(".log");
}

bool IsTableOperation(std::string_view operation, std::string_view verb) {
    return operation.starts_with(verb) && operation.ends_with(".ldb");
}

FaultScenarioSummary MutationScenario(FaultScenario scenario) {
    FaultHarness harness;
    std::unique_ptr<DatabaseEngine> database = harness.Open();
    const bool needs_rotation =
        scenario == FaultScenario::OldWalClose ||
        scenario == FaultScenario::Scheduling;
    const BatchValues old_values =
        needs_rotation ? LargeOldValues() : SmallValues("old");
    const BatchValues new_values = SmallValues("new");
    RequireSuccess(WritePair(*database, old_values), "baseline write");

    const std::string name = FaultScenarioName(scenario);
    const std::string message = "injected " + name + " failure";
    std::optional<FailOnce> failure;
    Status failed;
    switch (scenario) {
        case FaultScenario::WalAppend:
            failure.emplace(
                [](std::string_view operation) {
                    return IsLogOperation(operation, "append ");
                },
                Error::Io(message));
            harness.file_system().SetOperationHook(std::ref(*failure));
            failed = WritePair(*database, new_values);
            break;
        case FaultScenario::WalSync:
            failure.emplace(
                [](std::string_view operation) {
                    return IsLogOperation(operation, "sync ");
                },
                Error::Io(message));
            harness.file_system().SetOperationHook(std::ref(*failure));
            failed = WritePair(*database, new_values, true);
            break;
        case FaultScenario::OldWalClose:
            failure.emplace(
                [](std::string_view operation) {
                    return IsLogOperation(operation, "close ");
                },
                Error::Io(message));
            harness.file_system().SetOperationHook(std::ref(*failure));
            failed = WritePair(*database, new_values);
            break;
        case FaultScenario::TableSync:
            RequireSuccess(WritePair(*database, new_values), "acknowledged write");
            failure.emplace(
                [](std::string_view operation) {
                    return IsTableOperation(operation, "sync ");
                },
                Error::Io(message));
            harness.file_system().SetOperationHook(std::ref(*failure));
            failed = Flush(harness, *database);
            break;
        case FaultScenario::TableClose:
            RequireSuccess(WritePair(*database, new_values), "acknowledged write");
            failure.emplace(
                [](std::string_view operation) {
                    return IsTableOperation(operation, "close ");
                },
                Error::Io(message));
            harness.file_system().SetOperationHook(std::ref(*failure));
            failed = Flush(harness, *database);
            break;
        case FaultScenario::TableDirectorySync:
            RequireSuccess(WritePair(*database, new_values), "acknowledged write");
            failure.emplace(
                [seen = 0](std::string_view operation) mutable {
                    if (operation != "sync_directory db") {
                        return false;
                    }
                    ++seen;
                    return seen == 2;
                },
                Error::Io(message));
            harness.file_system().SetOperationHook(std::ref(*failure));
            failed = Flush(harness, *database);
            break;
        case FaultScenario::ManifestAppend:
            RequireSuccess(WritePair(*database, new_values), "acknowledged write");
            failure.emplace(
                [](std::string_view operation) {
                    return operation.starts_with("append MANIFEST-");
                },
                Error::Io(message));
            harness.file_system().SetOperationHook(std::ref(*failure));
            failed = Flush(harness, *database);
            break;
        case FaultScenario::ManifestSync:
            RequireSuccess(WritePair(*database, new_values), "acknowledged write");
            failure.emplace(
                [](std::string_view operation) {
                    return operation.starts_with("sync MANIFEST-");
                },
                Error::Io(message));
            harness.file_system().SetOperationHook(std::ref(*failure));
            failed = Flush(harness, *database);
            break;
        case FaultScenario::Scheduling:
            harness.executor().Reject(Error::Busy(message));
            failed = WritePair(*database, new_values);
            harness.executor().Reject(std::nullopt);
            break;
        case FaultScenario::Read:
            throw std::invalid_argument("read is not a mutation fault scenario");
    }
    harness.file_system().SetOperationHook({});
    if (failed.has_value()) {
        throw std::runtime_error("mutation fault unexpectedly succeeded");
    }

    const Result<DatabaseEngineState> state = database->GetState();
    if (!state.has_value()) {
        throw std::runtime_error("cannot inspect mutation-fault state: " +
                                 state.error().ToString());
    }
    if (!state->sticky_error.has_value()) {
        throw std::runtime_error("mutation fault did not become sticky");
    }
    if (state->sticky_error->code() != failed.error().code() ||
        state->sticky_error->message() != failed.error().message()) {
        throw std::runtime_error("mutation fault retained a different sticky error");
    }

    const std::size_t before_later = harness.file_system().operations().size();
    const Status later = WritePair(*database, SmallValues("later"));
    const std::size_t after_later = harness.file_system().operations().size();
    const bool reused_without_io =
        !later.has_value() &&
        later.error().code() == failed.error().code() &&
        later.error().message() == failed.error().message() &&
        before_later == after_later;
    const RecoveredBatchState recovered =
        RecoverBatch(harness, database, old_values, new_values);

    return {
        scenario,
        name,
        scenario == FaultScenario::Scheduling
            ? "schedule background"
            : failure->operation(),
        failed.error().code(),
        std::string(failed.error().message()),
        true,
        reused_without_io,
        false,
        recovered,
    };
}

}  // namespace

std::vector<FaultScenario> EnumerateFaultScenarios() {
    return {
        FaultScenario::Read,
        FaultScenario::WalAppend,
        FaultScenario::WalSync,
        FaultScenario::OldWalClose,
        FaultScenario::TableSync,
        FaultScenario::TableClose,
        FaultScenario::TableDirectorySync,
        FaultScenario::ManifestAppend,
        FaultScenario::ManifestSync,
        FaultScenario::Scheduling,
    };
}

std::string FaultScenarioName(FaultScenario scenario) {
    switch (scenario) {
        case FaultScenario::Read:
            return "read";
        case FaultScenario::WalAppend:
            return "wal-append";
        case FaultScenario::WalSync:
            return "wal-sync";
        case FaultScenario::OldWalClose:
            return "old-wal-close";
        case FaultScenario::TableSync:
            return "table-sync";
        case FaultScenario::TableClose:
            return "table-close";
        case FaultScenario::TableDirectorySync:
            return "table-directory-sync";
        case FaultScenario::ManifestAppend:
            return "manifest-append";
        case FaultScenario::ManifestSync:
            return "manifest-sync";
        case FaultScenario::Scheduling:
            return "scheduling";
    }
    throw std::invalid_argument("unknown fault scenario");
}

FaultScenarioSummary RunFaultScenario(FaultScenario scenario) {
    if (scenario == FaultScenario::Read) {
        return ReadScenario();
    }
    return MutationScenario(scenario);
}

}  // namespace modern_leveldb::stress
