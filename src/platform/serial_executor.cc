#include <cassert>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

#include "modern_leveldb/base/result.h"
#include "platform/background_executor.h"

namespace modern_leveldb {
namespace {

thread_local const SerialExecutor* CurrentSerialExecutor = nullptr;

}  // namespace

SerialExecutor::SerialExecutor()
    : worker_([this](std::stop_token stop_token) {
        CurrentSerialExecutor = this;
        worker_started_.count_down();
        Run(stop_token);
        CurrentSerialExecutor = nullptr;
      }) {
  worker_started_.wait();
}

SerialExecutor::~SerialExecutor() {
  const Status status = Shutdown();
  if (!status.has_value()) {
    std::terminate();
  }
}

Status SerialExecutor::Schedule(BackgroundTask task) {
  if (!task) {
    return std::unexpected(Error::InvalidArgument("background task is empty"));
  }

  auto queued_task = std::make_unique<BackgroundTask>(std::move(task));
  {
    std::lock_guard lock(queue_mutex_);
    if (!accepting_tasks_) {
      return std::unexpected(Error::Aborted("background executor is shut down"));
    }
    tasks_.push_back(std::move(queued_task));
  }
  work_available_.notify_one();
  return {};
}

Status SerialExecutor::Shutdown() {
  if (CurrentSerialExecutor == this) {
    return std::unexpected(
        Error::InvalidArgument("background executor cannot shut down from its worker"));
  }

  const std::thread::id caller = std::this_thread::get_id();
  {
    std::unique_lock shutdown_lock(shutdown_mutex_);
    if (shutdown_state_ == ShutdownState::Stopped) {
      return {};
    }
    if (shutdown_state_ == ShutdownState::Stopping) {
      if (shutdown_owner_ == caller) {
        return {};
      }
      shutdown_complete_.wait(shutdown_lock,
                              [this] { return shutdown_state_ == ShutdownState::Stopped; });
      return {};
    }
    shutdown_state_ = ShutdownState::Stopping;
    shutdown_owner_ = caller;
  }

  std::deque<std::unique_ptr<BackgroundTask>> canceled_tasks;
  {
    std::lock_guard queue_lock(queue_mutex_);
    accepting_tasks_ = false;
    canceled_tasks.swap(tasks_);
  }

  worker_.request_stop();
  work_available_.notify_all();
  worker_.join();
  canceled_tasks.clear();

  {
    std::lock_guard shutdown_lock(shutdown_mutex_);
    shutdown_state_ = ShutdownState::Stopped;
    shutdown_owner_ = {};
  }
  shutdown_complete_.notify_all();
  return {};
}

void SerialExecutor::Run(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    std::unique_ptr<BackgroundTask> task;
    {
      std::unique_lock lock(queue_mutex_);
      work_available_.wait(lock, stop_token, [this] { return !tasks_.empty(); });
      if (stop_token.stop_requested()) {
        return;
      }
      assert(!tasks_.empty());
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    (*task)(stop_token);
  }
}

}  // namespace modern_leveldb
