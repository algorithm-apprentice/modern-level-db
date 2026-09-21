#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <latch>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/result.h"
#include "platform/background_executor.h"

namespace modern_leveldb {
namespace {

struct ReentrantLifecycleState {
  SerialExecutor* executor;
  std::atomic<bool> armed = false;
  std::atomic<bool> schedule_succeeded = false;
  std::latch lifecycle_ran{1};
};

class ReentrantLifecycleCallable {
 public:
  explicit ReentrantLifecycleCallable(std::shared_ptr<ReentrantLifecycleState> state)
      : state_(std::move(state)) {}

  void operator()(std::stop_token) const {}

  ~ReentrantLifecycleCallable() {
    if (state_ != nullptr && state_->armed.exchange(false, std::memory_order_relaxed)) {
      state_->schedule_succeeded.store(
          state_->executor->Schedule([](std::stop_token) {}).has_value(),
          std::memory_order_relaxed);
      state_->lifecycle_ran.count_down();
    }
  }

 private:
  std::shared_ptr<ReentrantLifecycleState> state_;
};

template <typename T>
concept HasShutdown = requires(T& executor) { executor.Shutdown(); };

static_assert(std::is_abstract_v<BackgroundExecutor>);
static_assert(!std::is_copy_constructible_v<SerialExecutor>);
static_assert(!std::is_copy_assignable_v<SerialExecutor>);
static_assert(!std::is_move_constructible_v<SerialExecutor>);
static_assert(!std::is_move_assignable_v<SerialExecutor>);
static_assert(!HasShutdown<BackgroundExecutor>);
static_assert(!HasShutdown<SerialExecutor>);

TEST(SerialExecutorTest, RejectsEmptyTask) {
  SerialExecutor executor;

  const Status status = executor.Schedule({});

  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
}

TEST(SerialExecutorTest, ExecutesTasksInFifoOrderOnOneWorker) {
  SerialExecutor executor;
  const std::thread::id caller = std::this_thread::get_id();
  std::latch blocker_started(1);
  std::latch release_blocker(1);
  std::array<int, 3> order{};
  std::array<std::thread::id, 3> worker_ids{};
  std::atomic<std::size_t> next_index = 0;
  std::latch completed(3);

  ASSERT_TRUE(executor.Schedule([&](std::stop_token) {
    blocker_started.count_down();
    release_blocker.wait();
  }));
  blocker_started.wait();

  for (int value = 1; value <= 3; ++value) {
    ASSERT_TRUE(executor.Schedule([&, value](std::stop_token) {
      const std::size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
      order[index] = value;
      worker_ids[index] = std::this_thread::get_id();
      completed.count_down();
    }));
  }
  release_blocker.count_down();

  completed.wait();
  EXPECT_EQ(order, (std::array<int, 3>{1, 2, 3}));
  EXPECT_NE(worker_ids[0], caller);
  EXPECT_EQ(worker_ids[0], worker_ids[1]);
  EXPECT_EQ(worker_ids[1], worker_ids[2]);
}

TEST(SerialExecutorTest, OwnsCopiesOfScheduledCallables) {
  SerialExecutor executor;
  std::string value = "owned";
  std::string observed;
  std::latch completed(1);

  ASSERT_TRUE(executor.Schedule([value, &observed, &completed](std::stop_token) {
    observed = value;
    completed.count_down();
  }));
  value = "changed";

  completed.wait();
  EXPECT_EQ(observed, "owned");
}

TEST(SerialExecutorTest, AcceptsConcurrentScheduling) {
  constexpr int ProducerCount = 4;
  constexpr int TasksPerProducer = 50;
  constexpr int TaskCount = ProducerCount * TasksPerProducer;

  SerialExecutor executor;
  std::atomic<int> executed = 0;
  std::atomic<int> schedule_errors = 0;
  std::latch completed(TaskCount);
  std::vector<std::jthread> producers;
  producers.reserve(ProducerCount);

  for (int producer = 0; producer < ProducerCount; ++producer) {
    producers.emplace_back([&] {
      for (int task = 0; task < TasksPerProducer; ++task) {
        Status status = executor.Schedule([&](std::stop_token) {
          executed.fetch_add(1, std::memory_order_relaxed);
          completed.count_down();
        });
        if (!status.has_value()) {
          schedule_errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  producers.clear();

  completed.wait();
  EXPECT_EQ(schedule_errors.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(executed.load(std::memory_order_relaxed), TaskCount);
}

TEST(SerialExecutorTest, RunsCallableLifecycleOutsideQueueLock) {
  SerialExecutor executor;
  std::latch running(1);
  std::atomic<bool> release_running_task = false;

  ASSERT_TRUE(executor.Schedule([&](std::stop_token) {
    running.count_down();
    while (!release_running_task.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
  }));
  running.wait();

  auto state = std::make_shared<ReentrantLifecycleState>();
  state->executor = &executor;
  ASSERT_TRUE(executor.Schedule(ReentrantLifecycleCallable(state)));
  state->armed.store(true, std::memory_order_relaxed);
  release_running_task.store(true, std::memory_order_relaxed);

  state->lifecycle_ran.wait();
  EXPECT_TRUE(state->schedule_succeeded.load(std::memory_order_relaxed));
}

TEST(SerialExecutorTest, DestructorRequestsStopAndCancelsQueuedTasks) {
  std::latch running(1);
  std::atomic<bool> running_task_saw_stop = false;
  std::atomic<bool> queued_task_ran = false;
  auto executor = std::make_unique<SerialExecutor>();

  ASSERT_TRUE(executor->Schedule([&](std::stop_token stop_token) {
    running.count_down();
    while (!stop_token.stop_requested()) {
      std::this_thread::yield();
    }
    running_task_saw_stop.store(true, std::memory_order_relaxed);
  }));
  running.wait();
  ASSERT_TRUE(executor->Schedule(
      [&](std::stop_token) { queued_task_ran.store(true, std::memory_order_relaxed); }));

  executor.reset();

  EXPECT_TRUE(running_task_saw_stop.load(std::memory_order_relaxed));
  EXPECT_FALSE(queued_task_ran.load(std::memory_order_relaxed));
}

TEST(SerialExecutorTest, DestructorRunsStopCallbacksWithoutQueueLock) {
  std::latch callback_registered(1);
  std::atomic<int> callback_error_code = -1;
  auto executor = std::make_unique<SerialExecutor>();
  SerialExecutor* executor_pointer = executor.get();

  ASSERT_TRUE(executor->Schedule([&](std::stop_token stop_token) {
    std::stop_callback callback(stop_token, [&] {
      const Status status = executor_pointer->Schedule([](std::stop_token) {});
      if (!status.has_value()) {
        callback_error_code.store(static_cast<int>(status.error().code()),
                                  std::memory_order_relaxed);
      }
    });
    callback_registered.count_down();
    while (!stop_token.stop_requested()) {
      std::this_thread::yield();
    }
  }));
  callback_registered.wait();

  executor.reset();

  EXPECT_EQ(callback_error_code.load(std::memory_order_relaxed),
            static_cast<int>(ErrorCode::Aborted));
}

TEST(SerialExecutorTest, DestructorRunsCanceledTaskCleanupWithoutQueueLock) {
  std::latch running(1);
  std::atomic<int> cleanup_error_code = -1;
  auto executor = std::make_unique<SerialExecutor>();
  SerialExecutor* executor_pointer = executor.get();

  ASSERT_TRUE(executor->Schedule([&](std::stop_token stop_token) {
    running.count_down();
    while (!stop_token.stop_requested()) {
      std::this_thread::yield();
    }
  }));
  running.wait();

  auto cleanup = std::shared_ptr<int>(new int(1), [&](int* value) {
    delete value;
    const Status status = executor_pointer->Schedule([](std::stop_token) {});
    if (!status.has_value()) {
      cleanup_error_code.store(static_cast<int>(status.error().code()), std::memory_order_relaxed);
    }
  });
  ASSERT_TRUE(executor->Schedule([cleanup](std::stop_token) {}));
  cleanup.reset();

  executor.reset();

  EXPECT_EQ(cleanup_error_code.load(std::memory_order_relaxed),
            static_cast<int>(ErrorCode::Aborted));
}

}  // namespace
}  // namespace modern_leveldb
