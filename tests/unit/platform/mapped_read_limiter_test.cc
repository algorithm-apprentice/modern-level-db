#include "platform/mapped_read_limiter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <latch>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>

namespace modern_leveldb {
namespace {

TEST(MappedReadLimiterTest, AdmitsCapacityAndRestoresRejectedAttempts) {
    MappedReadLimiter limiter(2);

    EXPECT_TRUE(limiter.Acquire());
    EXPECT_TRUE(limiter.Acquire());
    EXPECT_FALSE(limiter.Acquire());
    EXPECT_FALSE(limiter.Acquire());

    limiter.Release();
    EXPECT_TRUE(limiter.Acquire());
    limiter.Release();
    limiter.Release();

    EXPECT_TRUE(limiter.Acquire());
    EXPECT_TRUE(limiter.Acquire());
    EXPECT_FALSE(limiter.Acquire());
    limiter.Release();
    limiter.Release();
}

TEST(MappedReadLimiterTest, ReservesOnlyConfiguredSlotsConcurrently) {
    constexpr std::size_t Capacity = 3;
    constexpr std::size_t WorkerCount = 16;
    MappedReadLimiter limiter(Capacity);
    std::latch ready(WorkerCount);
    std::latch start(1);
    std::latch attempted(WorkerCount);
    std::latch release(1);
    std::atomic<std::size_t> acquired = 0;
    std::vector<std::jthread> workers;
    workers.reserve(WorkerCount);

    for (std::size_t worker = 0; worker < WorkerCount; ++worker) {
        workers.emplace_back([&] {
            ready.count_down();
            start.wait();
            const bool admitted = limiter.Acquire();
            if (admitted) {
                acquired.fetch_add(1, std::memory_order_relaxed);
            }
            attempted.count_down();
            if (admitted) {
                release.wait();
                limiter.Release();
            }
        });
    }

    ready.wait();
    start.count_down();
    attempted.wait();
    EXPECT_EQ(acquired.load(std::memory_order_relaxed), Capacity);
    release.count_down();
    workers.clear();

    for (std::size_t slot = 0; slot < Capacity; ++slot) {
        EXPECT_TRUE(limiter.Acquire());
    }
    EXPECT_FALSE(limiter.Acquire());
    for (std::size_t slot = 0; slot < Capacity; ++slot) {
        limiter.Release();
    }
}

TEST(MappedReadLimiterTest, RejectsCountsOutsideTheSignedAtomicDomain) {
    EXPECT_THROW(static_cast<void>(
                     std::make_unique<MappedReadLimiter>(std::numeric_limits<std::size_t>::max())),
                 std::bad_array_new_length);
}

}  // namespace
}  // namespace modern_leveldb
