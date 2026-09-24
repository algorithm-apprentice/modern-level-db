#ifndef MODERN_LEVELDB_TESTS_SUPPORT_MANUAL_CLOCK_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_MANUAL_CLOCK_H_

#include <mutex>
#include <stop_token>
#include <vector>

#include "platform/clock.h"

namespace modern_leveldb::test_support {

// A clock whose sleeps return at once and only advance its time. It records
// every sleep.
class ManualClock final : public Clock {
 public:
  [[nodiscard]] TimePoint Now() const noexcept override {
    const std::lock_guard lock(mutex_);
    return now_;
  }

  [[nodiscard]] bool SleepFor(Duration duration, std::stop_token /*stop_token*/) override {
    const std::lock_guard lock(mutex_);
    sleeps_.push_back(duration);
    now_ += duration;
    return true;
  }

  [[nodiscard]] std::vector<Duration> sleeps() const {
    const std::lock_guard lock(mutex_);
    return sleeps_;
  }

 private:
  mutable std::mutex mutex_;
  TimePoint now_;
  std::vector<Duration> sleeps_;
};

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_MANUAL_CLOCK_H_
