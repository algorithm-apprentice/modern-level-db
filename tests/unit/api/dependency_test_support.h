#ifndef MODERN_LEVELDB_TESTS_API_DEPENDENCY_TEST_SUPPORT_H_
#define MODERN_LEVELDB_TESTS_API_DEPENDENCY_TEST_SUPPORT_H_

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "modern_leveldb/background_executor.h"
#include "modern_leveldb/clock.h"
#include "modern_leveldb/file_system.h"

namespace modern_leveldb::dependency_test_support {

class CountingClock final : public Clock {
public:
    TimePoint Now() const noexcept override {
        const std::lock_guard lock(mutex_);
        return now_;
    }

    bool SleepFor(Duration duration, std::stop_token stop_token = {}) override {
        if (stop_token.stop_requested()) {
            return false;
        }
        const std::lock_guard lock(mutex_);
        ++sleeps_;
        if (duration > Duration::zero()) {
            now_ += duration;
        }
        return true;
    }

    std::size_t sleep_count() const {
        const std::lock_guard lock(mutex_);
        return sleeps_;
    }

private:
    mutable std::mutex mutex_;
    TimePoint now_{};
    std::size_t sleeps_ = 0;
};

class QueuedExecutor final : public BackgroundExecutor {
public:
    Status Schedule(BackgroundTask task) override {
        if (!task) {
            return std::unexpected(Error::InvalidArgument("empty test task"));
        }
        const std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
        ++scheduled_;
        return {};
    }

    bool RunOne() {
        BackgroundTask task;
        {
            const std::lock_guard lock(mutex_);
            if (tasks_.empty()) {
                return false;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task(std::stop_token{});
        return true;
    }

    void RunAll() {
        while (RunOne()) {
        }
    }

    std::size_t scheduled() const {
        const std::lock_guard lock(mutex_);
        return scheduled_;
    }

private:
    mutable std::mutex mutex_;
    std::deque<BackgroundTask> tasks_;
    std::size_t scheduled_ = 0;
};

class DrainExecutor final {
public:
    explicit DrainExecutor(QueuedExecutor& executor) : executor_(executor) {}
    ~DrainExecutor() { executor_.RunAll(); }

private:
    QueuedExecutor& executor_;
};

class FailingFileSystem final : public FileSystem {
public:
    explicit FailingFileSystem(bool throws = false) : throws_(throws) {}

    int call_count() const noexcept { return calls_.load(); }

    Result<std::unique_ptr<SequentialFile>> OpenSequential(const std::filesystem::path&) override {
        return Fail<std::unique_ptr<SequentialFile>>();
    }
    Result<std::unique_ptr<RandomAccessFile>> OpenRandomAccess(
        const std::filesystem::path&, std::optional<std::uint64_t>) override {
        return Fail<std::unique_ptr<RandomAccessFile>>();
    }
    Result<std::unique_ptr<WritableFile>> OpenWritable(const std::filesystem::path&) override {
        return Fail<std::unique_ptr<WritableFile>>();
    }
    Result<std::unique_ptr<WritableFile>> OpenAppendable(const std::filesystem::path&) override {
        return Fail<std::unique_ptr<WritableFile>>();
    }
    Result<bool> FileExists(const std::filesystem::path&) const override { return Fail<bool>(); }
    Result<std::vector<std::filesystem::path>> ListDirectory(
        const std::filesystem::path&) const override {
        return Fail<std::vector<std::filesystem::path>>();
    }
    Result<std::uint64_t> FileSize(const std::filesystem::path&) const override {
        return Fail<std::uint64_t>();
    }
    Status CreateDir(const std::filesystem::path&) override { return Fail<void>(); }
    Status RemoveFile(const std::filesystem::path&) override { return Fail<void>(); }
    Status RemoveDir(const std::filesystem::path&) override { return Fail<void>(); }
    Status RenameFile(const std::filesystem::path&, const std::filesystem::path&) override {
        return Fail<void>();
    }
    Status SyncDirectory(const std::filesystem::path&) override { return Fail<void>(); }
    Result<std::unique_ptr<FileLock>> LockFile(const std::filesystem::path&) override {
        return Fail<std::unique_ptr<FileLock>>();
    }

private:
    template <typename T>
    Result<T> Fail() const {
        ++calls_;
        if (throws_) {
            throw std::runtime_error("injected filesystem exception");
        }
        return std::unexpected(Error::Io("injected filesystem failure"));
    }

    bool throws_;
    mutable std::atomic<int> calls_ = 0;
};

}  // namespace modern_leveldb::dependency_test_support

#endif  // MODERN_LEVELDB_TESTS_API_DEPENDENCY_TEST_SUPPORT_H_
