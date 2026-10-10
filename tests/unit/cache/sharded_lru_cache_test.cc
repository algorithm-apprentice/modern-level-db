#include "cache/sharded_lru_cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/hash.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

using Cache = ShardedLruCache<int>;
using Handle = Cache::Handle;

struct CallbackValue {
    CallbackValue(int stored_value, std::function<void()> callback)
        : value(stored_value), on_destroy(std::move(callback)) {}

    int value;
    std::function<void()> on_destroy;

    ~CallbackValue() {
        if (on_destroy) {
            on_destroy();
        }
    }
};

using CallbackCache = ShardedLruCache<CallbackValue>;

static_assert(!std::is_copy_constructible_v<Cache>);
static_assert(!std::is_copy_assignable_v<Cache>);
static_assert(!std::is_move_constructible_v<Cache>);
static_assert(!std::is_move_assignable_v<Cache>);
static_assert(!std::is_copy_constructible_v<Handle>);
static_assert(!std::is_copy_assignable_v<Handle>);
static_assert(std::is_nothrow_move_constructible_v<Handle>);
static_assert(std::is_nothrow_move_assignable_v<Handle>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<Handle>())>>);

std::size_t Shard(ByteView key) { return Hash32(key, 0U) >> 28U; }

std::vector<std::string> KeysForShard(std::size_t shard, std::size_t count) {
    std::vector<std::string> keys;
    for (std::uint64_t candidate = 0; keys.size() < count; ++candidate) {
        const std::string key = "key-" + std::to_string(candidate);
        if (Shard(AsBytes(key)) == shard) {
            keys.push_back(key);
        }
    }
    return keys;
}

std::array<std::string, 16> OneKeyPerShard() {
    std::array<std::string, 16> keys;
    std::array<bool, 16> found{};
    std::size_t remaining = found.size();
    for (std::uint64_t candidate = 0; remaining > 0; ++candidate) {
        const std::string key = "shard-" + std::to_string(candidate);
        const std::size_t shard = Shard(AsBytes(key));
        if (!found[shard]) {
            found[shard] = true;
            keys[shard] = key;
            --remaining;
        }
    }
    return keys;
}

TEST(ShardedLruCacheTest, InsertsLooksUpAndErasesBinaryKeys) {
    Cache cache(16);
    const std::array binary_key{std::byte{0x00}, std::byte{0xff}, std::byte{0x10}};

    EXPECT_FALSE(cache.Lookup(binary_key).has_value());
    auto inserted = cache.Insert(binary_key, std::make_unique<const int>(42), 1);
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(**inserted, 42);

    auto found = cache.Lookup(binary_key);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(**found, 42);

    cache.Erase(binary_key);
    EXPECT_FALSE(cache.Lookup(binary_key).has_value());
    EXPECT_EQ(**inserted, 42);
    EXPECT_EQ(**found, 42);
}

TEST(ShardedLruCacheTest, SupportsEmptyKeys) {
    Cache cache(16);

    auto inserted = cache.Insert({}, std::make_unique<const int>(7), 1);

    ASSERT_TRUE(inserted.has_value());
    auto found = cache.Lookup({});
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(**found, 7);
}

TEST(ShardedLruCacheTest, RejectsNullValuesAndZeroCharge) {
    Cache cache(16);

    const auto null_value = cache.Insert(AsBytes("null"), std::unique_ptr<const int>{}, 1);
    ASSERT_FALSE(null_value.has_value());
    EXPECT_EQ(null_value.error().code(), ErrorCode::InvalidArgument);

    const auto zero_charge = cache.Insert(AsBytes("zero"), std::make_unique<const int>(1), 0);
    ASSERT_FALSE(zero_charge.has_value());
    EXPECT_EQ(zero_charge.error().code(), ErrorCode::InvalidArgument);
}

TEST(ShardedLruCacheTest, ReplacementKeepsOldHandleAlive) {
    Cache cache(160);
    const auto keys = KeysForShard(0, 1);
    auto old_handle = cache.Insert(AsBytes(keys[0]), std::make_unique<const int>(1), 3);
    ASSERT_TRUE(old_handle.has_value());
    EXPECT_EQ(cache.total_charge(), 3U);

    auto new_handle = cache.Insert(AsBytes(keys[0]), std::make_unique<const int>(2), 5);

    ASSERT_TRUE(new_handle.has_value());
    EXPECT_EQ(**old_handle, 1);
    EXPECT_EQ(**new_handle, 2);
    EXPECT_EQ(cache.total_charge(), 5U);
    auto found = cache.Lookup(AsBytes(keys[0]));
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(**found, 2);

    cache.Erase(AsBytes(keys[0]));
    EXPECT_EQ(cache.total_charge(), 0U);
    EXPECT_EQ(**old_handle, 1);
    EXPECT_EQ(**new_handle, 2);
}

TEST(ShardedLruCacheTest, GrowsItsHashTableWithinOneShard) {
    Cache cache(16 * 128);
    const auto keys = KeysForShard(0, 64);

    for (std::size_t index = 0; index < keys.size(); ++index) {
        auto inserted = cache.Insert(AsBytes(keys[index]),
                                     std::make_unique<const int>(static_cast<int>(index)), 1);
        ASSERT_TRUE(inserted.has_value());
    }

    for (std::size_t index = 0; index < keys.size(); ++index) {
        auto found = cache.Lookup(AsBytes(keys[index]));
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(**found, static_cast<int>(index));
    }
}

TEST(ShardedLruCacheTest, DistinguishesDifferentKeysWithTheSameHash) {
    constexpr std::string_view First = "collision-3728";
    constexpr std::string_view Second = "collision-161100";
    ASSERT_EQ(Hash32(AsBytes(First), 0U), Hash32(AsBytes(Second), 0U));
    Cache cache(32);
    ASSERT_TRUE(cache.Insert(AsBytes(First), std::make_unique<const int>(1), 1).has_value());
    ASSERT_TRUE(cache.Insert(AsBytes(Second), std::make_unique<const int>(2), 1).has_value());

    auto first = cache.Lookup(AsBytes(First));
    auto second = cache.Lookup(AsBytes(Second));

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(**first, 1);
    EXPECT_EQ(**second, 2);
    cache.Erase(AsBytes(First));
    EXPECT_FALSE(cache.Lookup(AsBytes(First)).has_value());
    EXPECT_TRUE(cache.Lookup(AsBytes(Second)).has_value());
}

TEST(ShardedLruCacheTest, MoveAssignmentReleasesThePreviousPin) {
    Cache cache(16);
    const auto keys = KeysForShard(0, 3);
    auto first = cache.Insert(AsBytes(keys[0]), std::make_unique<const int>(1), 1).value();
    auto second = cache.Insert(AsBytes(keys[1]), std::make_unique<const int>(2), 1).value();

    first = std::move(second);
    EXPECT_EQ(*first, 2);
    Handle* alias = &first;
    first = std::move(*alias);
    EXPECT_EQ(*first, 2);
    auto third = cache.Insert(AsBytes(keys[2]), std::make_unique<const int>(3), 1);
    ASSERT_TRUE(third.has_value());

    EXPECT_FALSE(cache.Lookup(AsBytes(keys[0])).has_value());
    EXPECT_TRUE(cache.Lookup(AsBytes(keys[1])).has_value());
    EXPECT_TRUE(cache.Lookup(AsBytes(keys[2])).has_value());
}

TEST(ShardedLruCacheTest, DestroysValuesAfterCacheAndHandleOwnershipEnd) {
    CallbackCache cache(16);
    std::atomic<bool> destroyed = false;
    auto handle = cache.Insert(AsBytes("key"),
                               std::make_unique<const CallbackValue>(
                                   42, [&] { destroyed.store(true, std::memory_order_relaxed); }),
                               1);
    ASSERT_TRUE(handle.has_value());
    std::optional<CallbackCache::Handle> pinned(std::move(*handle));

    cache.Erase(AsBytes("key"));
    EXPECT_FALSE(destroyed.load(std::memory_order_relaxed));

    pinned.reset();
    EXPECT_TRUE(destroyed.load(std::memory_order_relaxed));
}

TEST(ShardedLruCacheTest, EvictsLeastRecentlyUsedUnpinnedEntry) {
    Cache cache(32);
    const auto keys = KeysForShard(0, 3);
    {
        auto first = cache.Insert(AsBytes(keys[0]), std::make_unique<const int>(1), 1);
        auto second = cache.Insert(AsBytes(keys[1]), std::make_unique<const int>(2), 1);
        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
    }

    auto refreshed = cache.Lookup(AsBytes(keys[0]));
    ASSERT_TRUE(refreshed.has_value());
    refreshed.reset();
    auto third = cache.Insert(AsBytes(keys[2]), std::make_unique<const int>(3), 1);
    ASSERT_TRUE(third.has_value());

    EXPECT_TRUE(cache.Lookup(AsBytes(keys[0])).has_value());
    EXPECT_FALSE(cache.Lookup(AsBytes(keys[1])).has_value());
    EXPECT_TRUE(cache.Lookup(AsBytes(keys[2])).has_value());
}

TEST(ShardedLruCacheTest, PinnedEntriesMayExceedCapacity) {
    Cache cache(16);
    const auto keys = KeysForShard(0, 3);
    std::optional<Handle> first =
        cache.Insert(AsBytes(keys[0]), std::make_unique<const int>(1), 1).value();
    std::optional<Handle> second =
        cache.Insert(AsBytes(keys[1]), std::make_unique<const int>(2), 1).value();

    EXPECT_TRUE(cache.Lookup(AsBytes(keys[0])).has_value());
    EXPECT_TRUE(cache.Lookup(AsBytes(keys[1])).has_value());
    EXPECT_EQ(cache.total_charge(), 2U);

    first.reset();
    auto third = cache.Insert(AsBytes(keys[2]), std::make_unique<const int>(3), 1);
    ASSERT_TRUE(third.has_value());
    EXPECT_FALSE(cache.Lookup(AsBytes(keys[0])).has_value());
    EXPECT_TRUE(cache.Lookup(AsBytes(keys[1])).has_value());
    EXPECT_TRUE(cache.Lookup(AsBytes(keys[2])).has_value());
    second.reset();
}

TEST(ShardedLruCacheTest, ZeroCapacityDoesNotRetainEntries) {
    Cache cache(0);

    auto inserted = cache.Insert(AsBytes("key"), std::make_unique<const int>(9), 1);

    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(**inserted, 9);
    EXPECT_FALSE(cache.Lookup(AsBytes("key")).has_value());
    EXPECT_EQ(cache.total_charge(), 0U);
}

TEST(ShardedLruCacheTest, UsesLevelDbStyleCeilingCapacityPerShard) {
    Cache cache(1);
    const auto keys = OneKeyPerShard();

    for (std::size_t shard = 0; shard < keys.size(); ++shard) {
        auto inserted = cache.Insert(AsBytes(keys[shard]),
                                     std::make_unique<const int>(static_cast<int>(shard)), 1);
        ASSERT_TRUE(inserted.has_value());
    }

    for (std::size_t shard = 0; shard < keys.size(); ++shard) {
        auto found = cache.Lookup(AsBytes(keys[shard]));
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(**found, static_cast<int>(shard));
    }
    EXPECT_EQ(cache.total_charge(), 16U);
}

TEST(ShardedLruCacheTest, ChargeOverflowReturnsAnUncachedPinnedValue) {
    Cache cache(std::numeric_limits<std::size_t>::max());
    const auto keys = KeysForShard(0, 2);
    auto first = cache.Insert(AsBytes(keys[0]), std::make_unique<const int>(1),
                              std::numeric_limits<std::size_t>::max());
    ASSERT_TRUE(first.has_value());

    auto overflow = cache.Insert(AsBytes(keys[1]), std::make_unique<const int>(2), 1);

    ASSERT_TRUE(overflow.has_value());
    EXPECT_EQ(**overflow, 2);
    EXPECT_TRUE(cache.Lookup(AsBytes(keys[0])).has_value());
    EXPECT_FALSE(cache.Lookup(AsBytes(keys[1])).has_value());
    EXPECT_EQ(cache.total_charge(), std::numeric_limits<std::size_t>::max());
}

TEST(ShardedLruCacheTest, TotalChargeSaturatesAcrossShards) {
    Cache cache(std::numeric_limits<std::size_t>::max());
    const auto keys = OneKeyPerShard();
    const std::size_t charge = std::numeric_limits<std::size_t>::max() / 16U + 1U;
    std::vector<Handle> handles;
    handles.reserve(keys.size());

    for (const std::string& key : keys) {
        auto inserted = cache.Insert(AsBytes(key), std::make_unique<const int>(1), charge);
        ASSERT_TRUE(inserted.has_value());
        handles.push_back(std::move(*inserted));
    }

    EXPECT_EQ(cache.total_charge(), std::numeric_limits<std::size_t>::max());
}

TEST(ShardedLruCacheTest, AllocatesUniqueIdsConcurrently) {
    Cache cache(16);
    constexpr std::size_t ThreadCount = 4;
    constexpr std::size_t IdsPerThread = 100;
    std::array<std::vector<std::uint64_t>, ThreadCount> ids;
    std::vector<std::jthread> threads;

    for (std::size_t thread = 0; thread < ThreadCount; ++thread) {
        threads.emplace_back([&, thread] {
            ids[thread].reserve(IdsPerThread);
            for (std::size_t index = 0; index < IdsPerThread; ++index) {
                ids[thread].push_back(cache.NewId());
            }
        });
    }
    threads.clear();

    std::vector<std::uint64_t> all_ids;
    for (const auto& thread_ids : ids) {
        all_ids.insert(all_ids.end(), thread_ids.begin(), thread_ids.end());
    }
    std::ranges::sort(all_ids);
    EXPECT_EQ(all_ids.front(), 1U);
    EXPECT_EQ(all_ids.back(), ThreadCount * IdsPerThread);
    EXPECT_EQ(std::ranges::unique(all_ids).begin(), all_ids.end());
}

TEST(ShardedLruCacheTest, SupportsConcurrentOperations) {
    Cache cache(1'024);
    std::atomic<int> errors = 0;
    std::vector<std::jthread> threads;

    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&, thread] {
            for (int index = 0; index < 1'000; ++index) {
                const std::string key =
                    "thread-" + std::to_string(thread) + "-" + std::to_string(index % 50);
                auto inserted = cache.Insert(AsBytes(key), std::make_unique<const int>(index), 1);
                if (!inserted.has_value()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
                auto found = cache.Lookup(AsBytes(key));
                if (!found.has_value()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
                if (index % 7 == 0) {
                    cache.Erase(AsBytes(key));
                }
            }
        });
    }
    threads.clear();

    EXPECT_EQ(errors.load(std::memory_order_relaxed), 0);
}

TEST(ShardedLruCacheTest, DestroysEvictedValuesOutsideShardLock) {
    CallbackCache cache(16);
    const auto keys = KeysForShard(0, 2);
    std::atomic<bool> deleter_ran = false;

    auto first_value = std::make_unique<const CallbackValue>(1, [&] {
        cache.Erase(AsBytes(keys[1]));
        deleter_ran.store(true, std::memory_order_relaxed);
    });
    {
        auto first = cache.Insert(AsBytes(keys[0]), std::move(first_value), 1);
        ASSERT_TRUE(first.has_value());
    }

    auto second = cache.Insert(
        AsBytes(keys[1]), std::make_unique<const CallbackValue>(2, std::function<void()>{}), 1);

    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(deleter_ran.load(std::memory_order_relaxed));
    EXPECT_FALSE(cache.Lookup(AsBytes(keys[1])).has_value());
    EXPECT_EQ((*second)->value, 2);
}

}  // namespace
}  // namespace modern_leveldb
