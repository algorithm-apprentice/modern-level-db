#include "platform/clock.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <latch>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace modern_leveldb {
namespace {

using namespace std::chrono_literals;

static_assert(std::is_abstract_v<Clock>);
static_assert(std::is_final_v<SystemClock>);
static_assert(noexcept(std::declval<const SystemClock&>().Now()));

TEST(SystemClockTest, ReturnsNondecreasingTimePoints) {
  SystemClock clock;
  Clock::TimePoint previous = clock.Now();

  for (int sample = 0; sample < 1'000; ++sample) {
    const Clock::TimePoint current = clock.Now();
    EXPECT_GE(current, previous);
    previous = current;
  }
}

TEST(SystemClockTest, CompletesNonPositiveDurationsWithoutSleeping) {
  SystemClock clock;

  EXPECT_TRUE(clock.SleepFor(Clock::Duration::zero()));
  EXPECT_TRUE(clock.SleepFor(-1s));
}

TEST(SystemClockTest, SleepsForPositiveDurationWithoutStop) {
  SystemClock clock;
  const Clock::TimePoint start = clock.Now();

  EXPECT_TRUE(clock.SleepFor(1ms));

  EXPECT_GE(clock.Now() - start, 1ms);
}

TEST(SystemClockTest, RejectsSleepWhenStopWasAlreadyRequested) {
  SystemClock clock;
  std::stop_source stop_source;
  ASSERT_TRUE(stop_source.request_stop());

  EXPECT_FALSE(clock.SleepFor(1h, stop_source.get_token()));
  EXPECT_FALSE(clock.SleepFor(Clock::Duration::zero(), stop_source.get_token()));
}

TEST(SystemClockTest, InterruptsPositiveSleepWhenStopIsRequested) {
  SystemClock clock;
  std::stop_source stop_source;
  std::latch entered(1);
  std::atomic<bool> completed_sleep = true;

  std::jthread sleeper([&] {
    entered.count_down();
    completed_sleep.store(clock.SleepFor(1h, stop_source.get_token()), std::memory_order_relaxed);
  });

  entered.wait();
  ASSERT_TRUE(stop_source.request_stop());
  sleeper.join();

  EXPECT_FALSE(completed_sleep.load(std::memory_order_relaxed));
}

}  // namespace
}  // namespace modern_leveldb
