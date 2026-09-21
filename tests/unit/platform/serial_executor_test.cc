#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <latch>
#include <memory>
#include <semaphore>
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

using namespace std::chrono_literals;

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

static_assert(std::is_abstract_v<BackgroundExecutor>);
static_assert(!std::is_copy_constructible_v<SerialExecutor>);
static_assert(!std::is_copy_assignable_v<SerialExecutor>);
static_assert(!std::is_move_constructible_v<SerialExecutor>);
static_assert(!std::is_move_assignable_v<SerialExecutor>);

TEST(SerialExecutorTest, RejectsEmptyTask) {
  SerialExecutor executor;

  const Status status = executor.Schedule({});

  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
  ASSERT_TRUE(executor.Shutdown());
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
  ASSERT_TRUE(executor.Shutdown());

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
  ASSERT_TRUE(executor.Shutdown());
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
  ASSERT_TRUE(executor.Shutdown());
  EXPECT_EQ(schedule_errors.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(executed.load(std::memory_order_relaxed), TaskCount);
}

TEST(SerialExecutorTest, ShutdownRequestsStopAndCancelsQueuedTasks) {
  SerialExecutor executor;
  std::latch running(1);
  std::atomic<bool> running_task_saw_stop = false;
  std::atomic<bool> queued_task_ran = false;

  ASSERT_TRUE(executor.Schedule([&](std::stop_token stop_token) {
    running.count_down();
    while (!stop_token.stop_requested()) {
      std::this_thread::yield();
    }
    running_task_saw_stop.store(true, std::memory_order_relaxed);
  }));
  running.wait();
  ASSERT_TRUE(executor.Schedule(
      [&](std::stop_token) { queued_task_ran.store(true, std::memory_order_relaxed); }));

  ASSERT_TRUE(executor.Shutdown());

  EXPECT_TRUE(running_task_saw_stop.load(std::memory_order_relaxed));
  EXPECT_FALSE(queued_task_ran.load(std::memory_order_relaxed));
}

TEST(SerialExecutorTest, StopCallbackMayReenterShutdown) {
  SerialExecutor executor;
  std::latch callback_registered(1);
  std::atomic<bool> nested_shutdown_succeeded = false;

  ASSERT_TRUE(executor.Schedule([&](std::stop_token stop_token) {
    std::stop_callback callback(stop_token, [&] {
      nested_shutdown_succeeded.store(executor.Shutdown().has_value(), std::memory_order_relaxed);
    });
    callback_registered.count_down();
    while (!stop_token.stop_requested()) {
      std::this_thread::yield();
    }
  }));
  callback_registered.wait();

  ASSERT_TRUE(executor.Shutdown());
  EXPECT_TRUE(nested_shutdown_succeeded.load(std::memory_order_relaxed));
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
  ASSERT_TRUE(executor.Shutdown());
}

TEST(SerialExecutorTest, DestroysCanceledTasksAfterReleasingShutdownLock) {
  SerialExecutor executor;
  std::latch running(1);
  std::atomic<bool> cleanup_shutdown_succeeded = false;

  ASSERT_TRUE(executor.Schedule([&](std::stop_token stop_token) {
    running.count_down();
    while (!stop_token.stop_requested()) {
      std::this_thread::yield();
    }
  }));
  running.wait();

  auto cleanup = std::shared_ptr<int>(new int(1), [&](int* value) {
    delete value;
    cleanup_shutdown_succeeded.store(executor.Shutdown().has_value(), std::memory_order_relaxed);
  });
  ASSERT_TRUE(executor.Schedule([cleanup](std::stop_token) {}));
  cleanup.reset();

  ASSERT_TRUE(executor.Shutdown());
  EXPECT_TRUE(cleanup_shutdown_succeeded.load(std::memory_order_relaxed));
}

TEST(SerialExecutorTest, ConcurrentShutdownWaitsForCanceledTaskCleanup) {
  SerialExecutor executor;
  std::latch running(1);
  std::latch cleanup_started(1);
  std::latch release_cleanup(1);
  std::latch second_shutdown_entered(1);
  std::binary_semaphore second_shutdown_completed(0);

  ASSERT_TRUE(executor.Schedule([&](std::stop_token stop_token) {
    running.count_down();
    while (!stop_token.stop_requested()) {
      std::this_thread::yield();
    }
  }));
  running.wait();

  auto cleanup = std::shared_ptr<int>(new int(1), [&](int* value) {
    delete value;
    cleanup_started.count_down();
    release_cleanup.wait();
  });
  ASSERT_TRUE(executor.Schedule([cleanup](std::stop_token) {}));
  cleanup.reset();

  std::jthread first_shutdown([&] { (void)executor.Shutdown(); });
  cleanup_started.wait();
  std::jthread second_shutdown([&] {
    second_shutdown_entered.count_down();
    (void)executor.Shutdown();
    second_shutdown_completed.release();
  });

  second_shutdown_entered.wait();
  const bool completed_early = second_shutdown_completed.try_acquire_for(50ms);
  EXPECT_FALSE(completed_early);
  release_cleanup.count_down();
  if (!completed_early) {
    second_shutdown_completed.acquire();
  }
}

TEST(SerialExecutorTest, RejectsTasksAfterShutdown) {
  SerialExecutor executor;
  ASSERT_TRUE(executor.Shutdown());

  const Status status = executor.Schedule([](std::stop_token) {});

  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::Aborted);
}

TEST(SerialExecutorTest, RejectsShutdownFromWorkerThread) {
  SerialExecutor executor;
  std::atomic<int> error_code = -1;
  std::latch completed(1);

  ASSERT_TRUE(executor.Schedule([&](std::stop_token) {
    const Status status = executor.Shutdown();
    if (!status.has_value()) {
      error_code.store(static_cast<int>(status.error().code()), std::memory_order_relaxed);
    }
    completed.count_down();
  }));

  completed.wait();
  EXPECT_EQ(error_code.load(std::memory_order_relaxed),
            static_cast<int>(ErrorCode::InvalidArgument));
  ASSERT_TRUE(executor.Shutdown());
}

TEST(SerialExecutorTest, ShutdownIsIdempotent) {
  SerialExecutor executor;

  EXPECT_TRUE(executor.Shutdown());
  EXPECT_TRUE(executor.Shutdown());
}

TEST(SerialExecutorTest, AcceptsConcurrentShutdownCalls) {
  constexpr int CallerCount = 4;
  SerialExecutor executor;
  std::atomic<int> shutdown_errors = 0;
  std::vector<std::jthread> callers;
  callers.reserve(CallerCount);

  for (int caller = 0; caller < CallerCount; ++caller) {
    callers.emplace_back([&] {
      if (!executor.Shutdown().has_value()) {
        shutdown_errors.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  callers.clear();

  EXPECT_EQ(shutdown_errors.load(std::memory_order_relaxed), 0);
}

TEST(SerialExecutorTest, DestructorRequestsStopAndJoinsWorker) {
  std::latch running(1);
  std::atomic<bool> task_saw_stop = false;
  auto executor = std::make_unique<SerialExecutor>();

  ASSERT_TRUE(executor->Schedule([&](std::stop_token stop_token) {
    running.count_down();
    while (!stop_token.stop_requested()) {
      std::this_thread::yield();
    }
    task_saw_stop.store(true, std::memory_order_relaxed);
  }));
  running.wait();

  executor.reset();

  EXPECT_TRUE(task_saw_stop.load(std::memory_order_relaxed));
}

}  // namespace
}  // namespace modern_leveldb
