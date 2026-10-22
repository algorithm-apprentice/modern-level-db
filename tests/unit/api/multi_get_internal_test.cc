#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "api/api_internal.h"
#include "api/multi_get.h"
#include "format/write_batch.h"
#include "metadata/filenames.h"
#include "support/manual_executor.h"
#include "support/memory_file_system.h"
#include "table/block_cache.h"

namespace modern_leveldb {
namespace {

class ReleaseBatchLatch {
public:
    explicit ReleaseBatchLatch(std::latch& latch) noexcept : latch_(latch) {}
    ~ReleaseBatchLatch() { Open(); }
    void Open() noexcept {
        if (std::exchange(active_, false)) {
            latch_.count_down();
        }
    }

private:
    std::latch& latch_;
    bool active_ = true;
};

class ObservedBatchFileSystem final : public test_support::MemoryFileSystem {
    struct Observer {
        std::mutex mutex;
        std::function<void(MutableByteView)> before;
        std::function<void(MutableByteView)> after;
    };

    class File final : public RandomAccessFile {
    public:
        File(std::unique_ptr<RandomAccessFile> file, std::shared_ptr<Observer> observer)
            : file_(std::move(file)), observer_(std::move(observer)) {}
        Result<std::size_t> Read(std::uint64_t offset, MutableByteView output) const override {
            std::function<void(MutableByteView)> before;
            std::function<void(MutableByteView)> after;
            {
                const std::lock_guard lock(observer_->mutex);
                before = observer_->before;
                after = observer_->after;
            }
            struct Finish {
                std::function<void(MutableByteView)>& callback;
                MutableByteView output;
                ~Finish() {
                    if (callback) {
                        callback(output);
                    }
                }
            } finish{after, output};
            if (before) {
                before(output);
            }
            return file_->Read(offset, output);
        }

    private:
        std::unique_ptr<RandomAccessFile> file_;
        std::shared_ptr<Observer> observer_;
    };

public:
    void Observe(std::function<void(MutableByteView)> before,
                 std::function<void(MutableByteView)> after = {}) {
        const std::lock_guard lock(observer_->mutex);
        observer_->before = std::move(before);
        observer_->after = std::move(after);
    }
    Result<std::unique_ptr<RandomAccessFile>> OpenRandomAccess(
        const std::filesystem::path& path,
        std::optional<std::uint64_t> expected_size = std::nullopt) override {
        auto opened = MemoryFileSystem::OpenRandomAccess(path, expected_size);
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened).error());
        }
        return std::unique_ptr<RandomAccessFile>(
            std::make_unique<File>(std::move(*opened), observer_));
    }

private:
    std::shared_ptr<Observer> observer_ = std::make_shared<Observer>();
};

class ResetBatchObserver {
public:
    explicit ResetBatchObserver(ObservedBatchFileSystem& file_system) noexcept
        : file_system_(file_system) {}
    ~ResetBatchObserver() {
        file_system_.Observe({});
        file_system_.SetOperationHook({});
    }

private:
    ObservedBatchFileSystem& file_system_;
};

class InternalMultiGetTest : public testing::Test {
protected:
    void SetUp() override {
        DatabaseEngineOptions options;
        options.create_if_missing = true;
        options.file_system = &file_system_;
        options.executor = &executor_;
        options.block_cache = &blocks_;
        options.allow_mmap_reads = false;
        auto engine = DatabaseEngine::Open(options, "batch-db").value();
        state_ = std::make_shared<detail::DatabaseState>(Options{}, std::move(engine));
    }
    void TearDown() override {
        file_system_.Observe({});
        file_system_.SetOperationHook({});
        executor_.RunAll();
        state_.reset();
    }
    DatabaseEngine& engine() { return state_->engine(); }
    void Put(std::string_view key, std::string_view value) {
        EncodedWriteBatch batch;
        batch.Put(AsBytes(key), AsBytes(value));
        engine().Write(batch, false).value();
    }
    void Delete(std::string_view key) {
        EncodedWriteBatch batch;
        batch.Delete(AsBytes(key));
        engine().Write(batch, false).value();
    }
    void Flush() {
        auto flushed = std::async(std::launch::async, [&] { return engine().FlushMemTable(); });
        executor_.WaitForTask();
        executor_.RunAll();
        flushed.get().value();
    }
    static std::string Value(const OwningReadResult& result) {
        return std::string(AsStringView(result.value().value()));
    }

    ObservedBatchFileSystem file_system_;
    test_support::ManualExecutor executor_;
    BlockCache blocks_{0};
    std::shared_ptr<detail::DatabaseState> state_;
};

TEST_F(InternalMultiGetTest, EmptyAndSingleConcurrencyDoNotStartWorkers) {
    Put("a", "A");
    int starts = 0;
    const auto caller = std::this_thread::get_id();
    detail::MultiGetRuntime runtime;
    runtime.start = [&](std::function<void()>) -> std::jthread {
        ++starts;
        throw std::runtime_error("unexpected worker");
    };
    runtime.read = [&](DatabaseEngine& source, ByteView key,
                       const DatabaseEngineReadOptions& options) {
        EXPECT_EQ(std::this_thread::get_id(), caller);
        EXPECT_EQ(source.GetState().value().snapshot_count, 1U);
        return detail::ReadBatchKey(source, key, options);
    };
    EXPECT_TRUE(detail::ReadBatch(state_, {}, {}, nullptr, 4, runtime).empty());
    EXPECT_EQ(engine().GetState().value().snapshot_count, 0U);
    const std::array keys{AsBytes("a"), AsBytes("a")};
    const auto results = detail::ReadBatch(state_, keys, {}, nullptr, 1, runtime);
    ASSERT_EQ(results.size(), keys.size());
    EXPECT_EQ(Value(results[0]), "A");
    EXPECT_EQ(Value(results[1]), "A");
    EXPECT_EQ(starts, 0);
    EXPECT_EQ(engine().GetState().value().snapshot_count, 0U);
}

TEST_F(InternalMultiGetTest, PositionedReadsReallyOverlapWithinTheBoundWithDistinctBuffers) {
    for (const auto key : {"a", "b", "c", "d"}) {
        Put(key, key);
    }
    Flush();
    const std::array keys{AsBytes("a"), AsBytes("b"), AsBytes("c"), AsBytes("d")};
    std::atomic<int> active = 0;
    std::atomic<int> peak = 0;
    std::atomic<int> reached = 0;
    std::atomic<bool> duplicate = false;
    std::mutex mutex;
    std::set<const void*> destinations;
    std::latch entered(2);
    std::latch release(1);
    file_system_.Observe(
        [&](MutableByteView output) {
            const auto count = ++active;
            auto previous = peak.load();
            while (previous < count && !peak.compare_exchange_weak(previous, count)) {
            }
            {
                const std::lock_guard lock(mutex);
                if (!destinations.insert(output.data()).second) {
                    duplicate.store(true);
                }
            }
            if (reached.fetch_add(1) < 2) {
                entered.count_down();
            }
            release.wait();
        },
        [&](MutableByteView output) {
            {
                const std::lock_guard lock(mutex);
                destinations.erase(output.data());
            }
            --active;
        });
    std::future<std::vector<OwningReadResult>> reading;
    ReleaseBatchLatch finish(release);
    ResetBatchObserver reset(file_system_);
    reading = std::async(std::launch::async,
                         [&] { return detail::ReadBatch(state_, keys, {}, nullptr, 2); });
    entered.wait();
    EXPECT_EQ(active.load(), 2);
    EXPECT_EQ(engine().GetState().value().snapshot_count, 1U);
    finish.Open();
    const auto results = reading.get();
    ASSERT_EQ(results.size(), keys.size());
    for (std::size_t index = 0; index < keys.size(); ++index) {
        EXPECT_EQ(Value(results[index]), AsStringView(keys[index]));
    }
    EXPECT_EQ(peak.load(), 2);
    EXPECT_FALSE(duplicate.load());
    EXPECT_EQ(active.load(), 0);
    EXPECT_EQ(blocks_.total_charge(), 0U);
    EXPECT_EQ(engine().GetState().value().snapshot_count, 0U);
}

TEST_F(InternalMultiGetTest, ImplicitPinSpansAGapAfterReadSourcesReleaseAndARewritingCompaction) {
    Put("a", "A0");
    Put("b", "B0");
    Flush();
    std::filesystem::path old_table;
    const auto directory = file_system_.ListDirectory("batch-db").value();
    for (const auto& path : directory) {
        const auto parsed = ParseNativeFileName(path);
        if (parsed.has_value() && parsed->type == FileType::Table) {
            old_table = std::filesystem::path("batch-db") / path;
        }
    }
    ASSERT_FALSE(old_table.empty());
    const std::array keys{AsBytes("a"), AsBytes("b")};
    std::latch first_finished(1);
    std::latch continue_batch(1);
    detail::MultiGetRuntime runtime;
    runtime.read = [&](DatabaseEngine& source, ByteView key,
                       const DatabaseEngineReadOptions& options) {
        auto result = detail::ReadBatchKey(source, key, options);
        if (AsStringView(key) == "a") {
            first_finished.count_down();
            continue_batch.wait();
        }
        return result;
    };
    std::future<std::vector<OwningReadResult>> reading;
    ReleaseBatchLatch finish(continue_batch);
    reading = std::async(std::launch::async,
                         [&] { return detail::ReadBatch(state_, keys, {}, nullptr, 1, runtime); });
    first_finished.wait();
    EXPECT_EQ(engine().GetState().value().snapshot_count, 1U);
    EXPECT_EQ(engine().GetState().value().oldest_snapshot_sequence, 2U);
    Put("a", "A1");
    Delete("b");
    Flush();
    std::vector<std::byte> missing;
    for (int charge = 0; charge < 110; ++charge) {
        EXPECT_FALSE(engine().Get(AsBytes("aa"), missing).value());
    }
    ASSERT_GT(executor_.queued(), 0U);
    executor_.RunAll();
    // A registration preserves history, not old Version/file ownership.
    EXPECT_FALSE(file_system_.FileExists(old_table).value());
    EXPECT_EQ(engine().GetState().value().snapshot_count, 1U);
    finish.Open();
    const auto results = reading.get();
    ASSERT_EQ(results.size(), keys.size());
    EXPECT_EQ(Value(results[0]), "A0");
    EXPECT_EQ(Value(results[1]), "B0");
    EXPECT_EQ(engine().GetState().value().snapshot_count, 0U);
}

TEST_F(InternalMultiGetTest, OrdinaryPerKeyErrorsDoNotBecomeAbsenceOrCancelOtherPositions) {
    Put("a", "A");
    const std::array keys{AsBytes("a"), AsBytes("io"), AsBytes("missing"), AsBytes("corrupt"),
                          AsBytes("a")};
    detail::MultiGetRuntime runtime;
    runtime.read = [](DatabaseEngine& source, ByteView key,
                      const DatabaseEngineReadOptions& options) -> OwningReadResult {
        if (AsStringView(key) == "io") {
            return std::unexpected(Error::Io("one key failed"));
        }
        if (AsStringView(key) == "corrupt") {
            return std::unexpected(Error::Corruption("another key failed"));
        }
        return detail::ReadBatchKey(source, key, options);
    };
    const auto results = detail::ReadBatch(state_, keys, {}, nullptr, 4, runtime);
    ASSERT_EQ(results.size(), keys.size());
    EXPECT_EQ(Value(results[0]), "A");
    ASSERT_FALSE(results[1].has_value());
    EXPECT_EQ(results[1].error().code(), ErrorCode::Io);
    ASSERT_TRUE(results[2].has_value());
    EXPECT_FALSE(results[2]->has_value());
    ASSERT_FALSE(results[3].has_value());
    EXPECT_EQ(results[3].error().code(), ErrorCode::Corruption);
    EXPECT_EQ(Value(results[4]), "A");
    EXPECT_EQ(engine().GetState().value().snapshot_count, 0U);
}

TEST_F(InternalMultiGetTest, ActualEngineReadFailuresKeepTheirCodesAndOtherKeyResults) {
    Put("a", "A");
    Put("b", "B");
    Put("c", "C");
    Flush();
    std::vector<std::byte> warmed;
    ASSERT_TRUE(engine().Get(AsBytes("a"), warmed).value());
    std::size_t reads = 0;
    file_system_.SetOperationHook([&](std::string_view operation) -> Status {
        if (operation.starts_with("read ") && operation.ends_with(".ldb")) {
            if (++reads == 1) {
                return std::unexpected(Error::Io("actual positioned read failed"));
            }
            if (reads == 2) {
                return std::unexpected(Error::Corruption("actual read reported corruption"));
            }
        }
        return {};
    });
    ResetBatchObserver reset(file_system_);
    const std::array keys{AsBytes("a"), AsBytes("b"), AsBytes("c"), AsBytes("missing"),
                          AsBytes("a")};
    const auto results = detail::ReadBatch(state_, keys, {}, nullptr, 1);
    ASSERT_EQ(results.size(), keys.size());
    ASSERT_FALSE(results[0].has_value());
    EXPECT_EQ(results[0].error().code(), ErrorCode::Io);
    ASSERT_FALSE(results[1].has_value());
    EXPECT_EQ(results[1].error().code(), ErrorCode::Corruption);
    EXPECT_EQ(Value(results[2]), "C");
    ASSERT_TRUE(results[3].has_value());
    EXPECT_FALSE(results[3]->has_value());
    EXPECT_EQ(Value(results[4]), "A");
    EXPECT_EQ(reads, 4U);
    EXPECT_FALSE(engine().GetState().value().sticky_error.has_value());
    EXPECT_EQ(engine().GetState().value().snapshot_count, 0U);
}

TEST_F(InternalMultiGetTest, ActualWorkerAllocationExceptionIsRethrownAfterJoining) {
    Put("a", "A");
    Put("b", "B");
    Flush();
    std::vector<std::byte> warmed;
    ASSERT_TRUE(engine().Get(AsBytes("a"), warmed).value());
    const auto caller = std::this_thread::get_id();
    std::atomic<int> worker_failures = 0;
    file_system_.SetOperationHook([&](std::string_view operation) -> Status {
        if (operation.starts_with("read ") && operation.ends_with(".ldb") &&
            std::this_thread::get_id() != caller) {
            ++worker_failures;
            throw std::bad_alloc();
        }
        return {};
    });
    ResetBatchObserver reset(file_system_);
    const std::array keys{AsBytes("a"), AsBytes("b")};
    EXPECT_THROW(static_cast<void>(detail::ReadBatch(state_, keys, {}, nullptr, 2)),
                 std::bad_alloc);
    EXPECT_EQ(worker_failures.load(), 1);
    EXPECT_EQ(engine().GetState().value().snapshot_count, 0U);
}

TEST_F(InternalMultiGetTest, CallerThreadExceptionReleasesItsImplicitPinWithoutStartingWorkers) {
    const std::array keys{AsBytes("a")};
    int starts = 0;
    detail::MultiGetRuntime runtime;
    runtime.start = [&](std::function<void()>) -> std::jthread {
        ++starts;
        throw std::logic_error("unexpected worker");
    };
    runtime.read = [](DatabaseEngine&, ByteView,
                      const DatabaseEngineReadOptions&) -> OwningReadResult {
        throw std::runtime_error("caller read failure");
    };
    EXPECT_THROW(static_cast<void>(detail::ReadBatch(state_, keys, {}, nullptr, 1, runtime)),
                 std::runtime_error);
    EXPECT_EQ(starts, 0);
    EXPECT_EQ(engine().GetState().value().snapshot_count, 0U);
}

TEST_F(InternalMultiGetTest, NonjoinableWorkerCannotPublishDefaultShapedPartialResults) {
    const std::array keys{AsBytes("a"), AsBytes("b")};
    detail::MultiGetRuntime runtime;
    runtime.start = [](std::function<void()>) { return std::jthread{}; };
    EXPECT_THROW(static_cast<void>(detail::ReadBatch(state_, keys, {}, nullptr, 2, runtime)),
                 std::logic_error);
    EXPECT_EQ(engine().GetState().value().snapshot_count, 0U);
}

TEST_F(InternalMultiGetTest, CompletedStartupSelectsTheLowestWorkerIndexExceptionAfterJoining) {
    const std::array keys{AsBytes("zero"), AsBytes("one"), AsBytes("two")};
    std::barrier simultaneous(3);
    std::atomic<int> finished = 0;
    detail::MultiGetRuntime runtime;
    runtime.read = [&](DatabaseEngine&, ByteView key,
                       const DatabaseEngineReadOptions&) -> OwningReadResult {
        simultaneous.arrive_and_wait();
        ++finished;
        throw std::runtime_error(std::string(AsStringView(key)));
    };
    try {
        static_cast<void>(detail::ReadBatch(state_, keys, {}, nullptr, 3, runtime));
        FAIL() << "worker exception was hidden";
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(std::string_view(error.what()), "zero");
    }
    EXPECT_EQ(finished.load(), 3);
    EXPECT_EQ(engine().GetState().value().snapshot_count, 0U);
}

TEST_F(InternalMultiGetTest, StartupFailureJoinsBlockedIoWithoutHoldingTheEngineMutex) {
    Put("a", "A");
    Put("b", "B");
    Put("c", "C");
    Put("d", "D");
    Put("e", "E");
    Put("f", "F");
    Flush();
    std::vector<std::byte> warmed;
    ASSERT_TRUE(engine().Get(AsBytes("a"), warmed).value());
    const std::array keys{AsBytes("a"), AsBytes("b"), AsBytes("c"),
                          AsBytes("d"), AsBytes("e"), AsBytes("f")};
    for (const bool worker_throws : {false, true}) {
        SCOPED_TRACE(worker_throws);
        std::latch blocked(1);
        std::latch startup_failed(1);
        std::latch release(1);
        std::atomic<int> reads = 0;
        file_system_.Observe([&](MutableByteView) {
            if (reads.fetch_add(1) == 0) {
                blocked.count_down();
            }
            release.wait();
            if (worker_throws) {
                throw std::runtime_error("worker failure during failed startup");
            }
        });
        int starts = 0;
        detail::MultiGetRuntime runtime;
        runtime.start = [&](std::function<void()> task) {
            if (++starts == 2) {
                blocked.wait();
                startup_failed.count_down();
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again),
                    "batch construction failure");
            }
            return detail::StartBatchWorker(std::move(task));
        };
        std::future<DatabaseEngineState> progress;
        std::future<std::vector<OwningReadResult>> reading;
        ReleaseBatchLatch finish(release);
        ResetBatchObserver reset(file_system_);
        reading = std::async(std::launch::async, [&] {
            return detail::ReadBatch(state_, keys, {}, nullptr, 3, runtime);
        });
        startup_failed.wait();
        EXPECT_EQ(reading.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
        progress = std::async(std::launch::async, [&] {
            Put("z", "writer progress");
            return engine().GetState().value();
        });
        ASSERT_EQ(progress.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        EXPECT_EQ(progress.get().snapshot_count, 1U);
        finish.Open();
        try {
            static_cast<void>(reading.get());
            FAIL() << "startup exception was hidden";
        } catch (const std::system_error& error) {
            EXPECT_EQ(error.code(),
                      std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        EXPECT_EQ(starts, 2);
        EXPECT_EQ(reads.load(), 1);
        EXPECT_EQ(engine().GetState().value().snapshot_count, 0U);
    }
}

}  // namespace
}  // namespace modern_leveldb
