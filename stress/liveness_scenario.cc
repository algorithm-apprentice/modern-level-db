#include "stress/option_fault_stress.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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

using test_support::Entries;
using test_support::ManualClock;
using test_support::ManualExecutor;
using test_support::MemoryFileSystem;
using test_support::Model;

class LivenessHarness final {
public:
    LivenessHarness() {
        options_.file_system = &file_system_;
        options_.executor = &executor_;
        options_.clock = &clock_;
        options_.create_if_missing = true;
        options_.write_buffer_size = 1;
        options_.max_file_size = 1;
    }

    std::unique_ptr<DatabaseEngine> Open() {
        Result<std::unique_ptr<DatabaseEngine>> opened =
            DatabaseEngine::Open(options_, directory_);
        if (!opened.has_value()) {
            throw std::runtime_error("cannot open liveness database: " +
                                     opened.error().ToString());
        }
        return std::move(*opened);
    }

    std::size_t Drain(DatabaseEngine& database) {
        const int ran = executor_.RunAll();
        const Status waited = database.WaitForBackgroundWork();
        if (!waited.has_value()) {
            throw std::runtime_error("liveness background work failed: " +
                                     waited.error().ToString());
        }
        if (executor_.queued() != 0) {
            throw std::runtime_error("liveness background work remained queued");
        }
        return static_cast<std::size_t>(ran);
    }

    std::size_t TableOutputs() const {
        std::size_t count = 0;
        for (const std::string& operation : file_system_.operations()) {
            if (operation.starts_with("open_writable ") &&
                operation.ends_with(".ldb")) {
                ++count;
            }
        }
        return count;
    }

private:
    MemoryFileSystem file_system_;
    ManualExecutor executor_;
    ManualClock clock_;
    DatabaseEngineOptions options_;
    const std::filesystem::path directory_ = "db";
};

void RequireSuccess(const Status& status, std::string_view operation) {
    if (!status.has_value()) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 status.error().ToString());
    }
}

std::string LivenessKey(std::size_t index) {
    std::string number = std::to_string(index);
    const std::size_t padding = number.size() < 12 ? 12 - number.size() : 0;
    return "live/" + std::string(padding, '0') + number;
}

std::string LivenessValue(std::uint64_t seed, std::size_t index) {
    std::uint64_t state =
        seed ^ (static_cast<std::uint64_t>(index) * 0x9e3779b97f4a7c15ULL);
    std::string value(2'048, '\0');
    for (char& byte : value) {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        byte = static_cast<char>((state * 0x2545f4914f6cdd1dULL) >> 56U);
    }
    return value;
}

void Put(DatabaseEngine& database, std::string_view key,
         std::string_view value) {
    EncodedWriteBatch batch;
    batch.Put(AsBytes(key), AsBytes(value));
    RequireSuccess(database.Write(batch, false), "liveness write");
}

void VerifyRead(DatabaseEngine& database, std::string_view key,
                std::string_view expected) {
    std::vector<std::byte> value;
    const Result<bool> found = database.Get(AsBytes(key), value);
    if (!found.has_value()) {
        throw std::runtime_error("liveness read failed: " +
                                 found.error().ToString());
    }
    if (!*found || AsStringView(value) != expected) {
        throw std::runtime_error("liveness point read differs from model");
    }
}

Entries Scan(DatabaseEngine& database, std::size_t maximum_entries) {
    std::unique_ptr<DbIterator> iterator = database.NewIterator();
    Status moved = iterator->SeekToFirst();
    Entries entries;
    while (moved.has_value() && iterator->valid()) {
        if (entries.size() >= maximum_entries) {
            throw std::runtime_error("liveness iterator did not terminate");
        }
        entries.emplace_back(AsStringView(iterator->key()),
                             AsStringView(iterator->value()));
        moved = iterator->Next();
    }
    if (!moved.has_value()) {
        throw std::runtime_error("liveness scan failed: " +
                                 moved.error().ToString());
    }
    return entries;
}

void VerifyScan(DatabaseEngine& database, const Model& model) {
    test_support::RequireModelEqual(
        Scan(database, model.size()), test_support::ExpectedEntries(model, false),
        "liveness scan differs from model");
}

void VerifyIdleState(DatabaseEngine& database) {
    const Result<DatabaseEngineState> state = database.GetState();
    if (!state.has_value()) {
        throw std::runtime_error("cannot inspect liveness state: " +
                                 state.error().ToString());
    }
    if (state->sticky_error.has_value()) {
        throw std::runtime_error("fault-free liveness retained a sticky error: " +
                                 state->sticky_error->ToString());
    }
    if (state->background_work_scheduled) {
        throw std::runtime_error("fault-free liveness left background work scheduled");
    }
}

}  // namespace

LivenessScenarioSummary RunLivenessScenario(std::uint64_t seed,
                                            std::size_t writes) {
    if (writes == 0) {
        throw std::invalid_argument("liveness writes must be positive");
    }

    LivenessHarness harness;
    std::unique_ptr<DatabaseEngine> database = harness.Open();
    Model model;
    std::size_t background_tasks = 0;
    std::size_t point_reads = 0;
    std::size_t scans = 0;
    for (std::size_t index = 0; index < writes; ++index) {
        std::string key = LivenessKey(index);
        std::string value = LivenessValue(seed, index);
        Put(*database, key, value);
        model.emplace(std::move(key), std::move(value));
        background_tasks += harness.Drain(*database);

        if (index % 17U == 0) {
            const auto& [selected_key, selected_value] = *model.rbegin();
            VerifyRead(*database, selected_key, selected_value);
            ++point_reads;
        }
        if (index % 97U == 0) {
            VerifyScan(*database, model);
            ++scans;
        }
    }

    VerifyIdleState(*database);
    VerifyScan(*database, model);
    ++scans;
    database.reset();

    database = harness.Open();
    background_tasks += harness.Drain(*database);
    VerifyIdleState(*database);
    VerifyScan(*database, model);
    ++scans;

    return {
        seed,
        writes,
        background_tasks,
        harness.TableOutputs(),
        point_reads,
        scans,
        model.size(),
        StableModelFingerprint(model),
    };
}

}  // namespace modern_leveldb::stress
