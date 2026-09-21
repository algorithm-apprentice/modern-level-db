#ifndef MODERN_LEVELDB_PLATFORM_BACKGROUND_EXECUTOR_H_
#define MODERN_LEVELDB_PLATFORM_BACKGROUND_EXECUTOR_H_

#include <condition_variable>
#include <deque>
#include <functional>
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

 private:
  void Run(std::stop_token stop_token);

  std::mutex queue_mutex_;
  std::condition_variable_any work_available_;
  std::deque<std::unique_ptr<BackgroundTask>> tasks_;
  bool accepting_tasks_ = true;

  std::jthread worker_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_BACKGROUND_EXECUTOR_H_
