#ifndef MODERN_LEVELDB_CLOCK_H_
#define MODERN_LEVELDB_CLOCK_H_

#include <chrono>
#include <stop_token>

namespace modern_leveldb {

// Monotonic time and interruptible throttling. Implementations must support
// concurrent calls when used by multiple database instances.
class Clock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration = std::chrono::steady_clock::duration;

    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;
    virtual ~Clock() = default;

    [[nodiscard]] virtual TimePoint Now() const noexcept = 0;
    // False means cancellation; a completed non-positive sleep returns true.
    [[nodiscard]] virtual bool SleepFor(Duration duration, std::stop_token stop_token = {}) = 0;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_CLOCK_H_
