#include "platform/clock.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace modern_leveldb {

Clock::TimePoint SystemClock::Now() const noexcept { return std::chrono::steady_clock::now(); }

bool SystemClock::SleepFor(Duration duration, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return false;
  }
  if (duration <= Duration::zero()) {
    return true;
  }

  std::mutex mutex;
  std::condition_variable_any condition;
  std::unique_lock lock(mutex);
  condition.wait_for(lock, stop_token, duration, [] { return false; });
  return !stop_token.stop_requested();
}

}  // namespace modern_leveldb
