#ifndef MODERN_LEVELDB_PLATFORM_BACKGROUND_EXECUTOR_H_
#define MODERN_LEVELDB_PLATFORM_BACKGROUND_EXECUTOR_H_

#include <condition_variable>
#include <deque>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

using BackgroundTask = std::function<void(std::stop_token)>;

class BackgroundExecutor {
 public:
  BackgroundExecutor() = default;
  BackgroundExecutor(const BackgroundExecutor&) = delete;
  BackgroundExecutor& operator=(const BackgroundExecutor&) = delete;
  BackgroundExecutor(BackgroundExecutor&&) = delete;
  BackgroundExecutor& operator=(BackgroundExecutor&&) = delete;
  virtual ~BackgroundExecutor() = default;

  [[nodiscard]] virtual Status Schedule(BackgroundTask task) = 0;
  [[nodiscard]] virtual Status Shutdown() = 0;
};

class SerialExecutor final : public BackgroundExecutor {
 public:
  SerialExecutor();

  SerialExecutor(const SerialExecutor&) = delete;
  SerialExecutor& operator=(const SerialExecutor&) = delete;
  SerialExecutor(SerialExecutor&&) = delete;
  SerialExecutor& operator=(SerialExecutor&&) = delete;

  ~SerialExecutor() override;

  [[nodiscard]] Status Schedule(BackgroundTask task) override;
  [[nodiscard]] Status Shutdown() override;

 private:
  enum class ShutdownState {
    Running,
    Stopping,
    Stopped,
  };

  void Run(std::stop_token stop_token);

  std::mutex queue_mutex_;
  std::condition_variable_any work_available_;
  std::deque<std::unique_ptr<BackgroundTask>> tasks_;
  bool accepting_tasks_ = true;

  std::mutex shutdown_mutex_;
  std::condition_variable shutdown_complete_;
  ShutdownState shutdown_state_ = ShutdownState::Running;
  std::thread::id shutdown_owner_;
  std::latch worker_started_{1};
  std::jthread worker_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_BACKGROUND_EXECUTOR_H_
