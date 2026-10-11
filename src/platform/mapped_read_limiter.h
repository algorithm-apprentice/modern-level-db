#ifndef MODERN_LEVELDB_PLATFORM_MAPPED_READ_LIMITER_H_
#define MODERN_LEVELDB_PLATFORM_MAPPED_READ_LIMITER_H_

#include <atomic>
#include <cstddef>
#include <limits>
#include <new>

namespace modern_leveldb {

class MappedReadLimiter final {
public:
    explicit MappedReadLimiter(std::size_t maximum_mappings)
        : available_(CheckedCount(maximum_mappings)) {}

    [[nodiscard]] bool Acquire() noexcept {
        const std::ptrdiff_t previous = available_.fetch_sub(1, std::memory_order_relaxed);
        if (previous > 0) {
            return true;
        }
        static_cast<void>(available_.fetch_add(1, std::memory_order_relaxed));
        return false;
    }

    void Release() noexcept {
        static_cast<void>(available_.fetch_add(1, std::memory_order_relaxed));
    }

private:
    // Failed acquisition briefly decrements below zero before restoring the slot,
    // so every admitted count must fit the signed atomic domain.
    [[nodiscard]] static std::ptrdiff_t CheckedCount(std::size_t count) {
        if (count > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
            throw std::bad_array_new_length();
        }
        return static_cast<std::ptrdiff_t>(count);
    }

    std::atomic<std::ptrdiff_t> available_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_MAPPED_READ_LIMITER_H_
