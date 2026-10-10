#ifndef MODERN_LEVELDB_PLATFORM_MAPPED_READ_LIMITER_H_
#define MODERN_LEVELDB_PLATFORM_MAPPED_READ_LIMITER_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <new>

namespace modern_leveldb {

class MappedReadLimiter final {
public:
    explicit MappedReadLimiter(std::size_t maximum_mappings)
        : maximum_mappings_(CheckedCount(maximum_mappings)), available_(maximum_mappings_) {}

    [[nodiscard]] bool Acquire() noexcept {
        const std::ptrdiff_t previous = available_.fetch_sub(1, std::memory_order_relaxed);
        if (previous > 0) {
            return true;
        }
        [[maybe_unused]] const std::ptrdiff_t before_restore =
            available_.fetch_add(1, std::memory_order_relaxed);
        assert(before_restore < maximum_mappings_);
        return false;
    }

    void Release() noexcept {
        [[maybe_unused]] const std::ptrdiff_t previous =
            available_.fetch_add(1, std::memory_order_relaxed);
        assert(previous < maximum_mappings_);
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

    [[maybe_unused]] const std::ptrdiff_t maximum_mappings_;
    std::atomic<std::ptrdiff_t> available_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_MAPPED_READ_LIMITER_H_
