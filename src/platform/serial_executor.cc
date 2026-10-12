#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

#include "modern_leveldb/base/result.h"
#include "platform/background_executor.h"

namespace modern_leveldb {

SerialExecutor::SerialExecutor()
    : worker_([this](std::stop_token stop_token) { Run(stop_token); }) {}

SerialExecutor::~SerialExecutor() {
    TaskQueue canceled_tasks = StopAcceptingAndDetachTasks();
    StopAndJoinWorker();
    canceled_tasks.clear();
}

SerialExecutor::TaskQueue SerialExecutor::StopAcceptingAndDetachTasks() {
    TaskQueue canceled_tasks;
    {
        std::lock_guard queue_lock(queue_mutex_);
        accepting_tasks_ = false;
        canceled_tasks.swap(tasks_);
    }
    return canceled_tasks;
}

void SerialExecutor::StopAndJoinWorker() {
    worker_.request_stop();
    work_available_.notify_all();
    worker_.join();
}

Status SerialExecutor::Schedule(BackgroundTask task) {
    if (!task) {
        return std::unexpected(Error::InvalidArgument("background task is empty"));
    }
    return Enqueue(std::make_unique<BackgroundTask>(std::move(task)));
}

Status SerialExecutor::Enqueue(TaskOwner task) {
    {
        std::lock_guard lock(queue_mutex_);
        if (!accepting_tasks_) {
            return std::unexpected(Error::Aborted("background executor is shut down"));
        }
        tasks_.push_back(std::move(task));
    }
    work_available_.notify_one();
    return {};
}

SerialExecutor::TaskOwner SerialExecutor::WaitForTask(std::stop_token stop_token) {
    std::unique_lock lock(queue_mutex_);
    work_available_.wait(lock, stop_token, [this] { return !tasks_.empty(); });
    if (stop_token.stop_requested()) {
        return {};
    }
    TaskOwner task = std::move(tasks_.front());
    tasks_.pop_front();
    return task;
}

void SerialExecutor::Run(std::stop_token stop_token) {
    while (TaskOwner task = WaitForTask(stop_token)) {
        (*task)(stop_token);
    }
}

}  // namespace modern_leveldb
