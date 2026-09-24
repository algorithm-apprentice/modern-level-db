#ifndef MODERN_LEVELDB_TESTS_SUPPORT_MANUAL_EXECUTOR_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_MANUAL_EXECUTOR_H_

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

#include "modern_leveldb/base/result.h"
#include "platform/background_executor.h"

namespace modern_leveldb::test_support {

// A background executor whose tasks run only when a test runs them, on the
// test's thread. It can reject tasks or throw when it schedules them.
class ManualExecutor final : public BackgroundExecutor {
 public:
  [[nodiscard]] Status Schedule(BackgroundTask task) override {
    const std::lock_guard lock(mutex_);
    if (throws_) {
      throw std::runtime_error("scheduling threw");
    }
    if (rejection_.has_value()) {
      return std::unexpected(*rejection_);
    }
    tasks_.push_back(std::move(task));
    changed_.notify_all();
    return {};
  }

  // Makes every later Schedule fail with the error, or accept tasks again.
  void Reject(std::optional<Error> error) {
    const std::lock_guard lock(mutex_);
    rejection_ = std::move(error);
  }

  // Makes every later Schedule throw, or stop throwing.
  void Throw(bool throws) {
    const std::lock_guard lock(mutex_);
    throws_ = throws;
  }

  // Runs queued tasks, including the ones they queue, until none is left, and
  // returns how many ran.
  int RunAll() {
    int ran = 0;
    while (RunOne()) {
      ++ran;
    }
    return ran;
  }

  // Runs the first queued task, if there is one, and returns whether one ran.
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
    task(std::stop_token());
    return true;
  }

  // Waits until a task is queued.
  void WaitForTask() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return !tasks_.empty(); });
  }

  [[nodiscard]] std::size_t queued() const {
    const std::lock_guard lock(mutex_);
    return tasks_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<BackgroundTask> tasks_;
  std::optional<Error> rejection_;
  bool throws_ = false;
};

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_MANUAL_EXECUTOR_H_
