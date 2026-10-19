#ifndef MODERN_LEVELDB_BACKGROUND_EXECUTOR_H_
#define MODERN_LEVELDB_BACKGROUND_EXECUTOR_H_

#include <functional>
#include <stop_token>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

using BackgroundTask = std::function<void(std::stop_token)>;

// Scheduling must support concurrent producers. Success accepts the task once
// for eventual execution, never inline in Schedule. Failure or an exception
// must not retain or execute it. Tasks must not let exceptions escape a thread.
// Accepted work must progress even while database destruction waits for it.
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

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BACKGROUND_EXECUTOR_H_
