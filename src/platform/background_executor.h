#ifndef MODERN_LEVELDB_PLATFORM_BACKGROUND_EXECUTOR_H_
#define MODERN_LEVELDB_PLATFORM_BACKGROUND_EXECUTOR_H_

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <source_location>
#include <stop_token>
#include <thread>

#include "modern_leveldb/background_executor.h"

namespace modern_leveldb {

// One worker runs queued callbacks in order. Destruction stops/joins the worker
// and discards unstarted callbacks; it does not drain the queue. Tasks must contain
// their own exceptions. Engine shutdown waits for its accepted work before teardown.
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
    using TaskOwner = std::unique_ptr<BackgroundTask>;
    using TaskQueue = std::deque<TaskOwner>;

    [[nodiscard]] Status Enqueue(TaskOwner task, std::source_location error_location);
    void StopAcceptingAndDetachTasks(TaskQueue& canceled_tasks);
    void StopAndJoinWorker();
    [[nodiscard]] TaskOwner WaitForTask(std::stop_token stop_token);
    void Run(std::stop_token stop_token);

    std::mutex queue_mutex_;
    std::condition_variable_any work_available_;
    TaskQueue tasks_;
    bool accepting_tasks_ = true;

    std::jthread worker_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_BACKGROUND_EXECUTOR_H_
