#ifndef MODERN_LEVELDB_PLATFORM_MAPPED_READ_LIMITER_H_
#define MODERN_LEVELDB_PLATFORM_MAPPED_READ_LIMITER_H_

#include <atomic>
#include <cassert>
#include <cstddef>

namespace modern_leveldb {

class MappedReadLimiter final {
public:
    explicit MappedReadLimiter(std::ptrdiff_t maximum_mappings) noexcept
        : maximum_mappings_(maximum_mappings), available_(maximum_mappings) {
        assert(maximum_mappings >= 0);
    }

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
    [[maybe_unused]] const std::ptrdiff_t maximum_mappings_;
    std::atomic<std::ptrdiff_t> available_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_MAPPED_READ_LIMITER_H_
