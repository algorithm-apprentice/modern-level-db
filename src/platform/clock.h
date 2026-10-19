#ifndef MODERN_LEVELDB_PLATFORM_CLOCK_H_
#define MODERN_LEVELDB_PLATFORM_CLOCK_H_

#include "modern_leveldb/clock.h"

namespace modern_leveldb {

class SystemClock final : public Clock {
public:
    [[nodiscard]] TimePoint Now() const noexcept override;
    [[nodiscard]] bool SleepFor(Duration duration, std::stop_token stop_token = {}) override;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_CLOCK_H_
