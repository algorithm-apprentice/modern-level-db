#include "table/clock_block_cache.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/hash.h"
#include "table/block.h"
#include "table/block_builder.h"

namespace modern_leveldb {
namespace {

using Key = std::array<std::byte, 16>;

Key KeyOf(std::uint64_t identity, std::uint64_t offset) {
    Key key{};
    EncodeFixed64(MutableByteView(key).first<8>(), identity);
    EncodeFixed64(MutableByteView(key).last<8>(), offset);
    return key;
}

std::unique_ptr<const Block> ValueOf(std::string_view value) {
    BlockBuilder builder(16);
    builder.Add(AsBytes("key"), AsBytes(value));
    const auto bytes = builder.Finish();
    auto block = Block::Create(std::vector<std::byte>(bytes.begin(), bytes.end())).value();
    return std::make_unique<const Block>(std::move(block));
}

std::string TextOf(const ClockBlockCache::Handle& handle) {
    Block::Iterator cursor(handle.value(), BytewiseComparator());
    cursor.SeekToFirst().value();
    return std::string(AsStringView(cursor.value()));
}

Key CollidingKey(std::uint32_t number) {
    constexpr std::uint32_t Multiplier = 0xc6a4a793U;
    constexpr std::uint32_t Inverse = 0xd8198e9bU;
    constexpr std::uint32_t Target = 0x01234567U;
    const std::array<std::uint32_t, 3> words{number, 0x5a5a1234U, number * 0x9e3779b9U};
    auto hash = std::uint32_t{16} * Multiplier;
    Key key{};
    for (std::size_t index = 0; index < words.size(); ++index) {
        EncodeFixed32(MutableByteView(key).subspan(index * 4, 4), words[index]);
        hash = (hash + words[index]) * Multiplier;
        hash ^= hash >> 16U;
    }
    const auto final_word = ((Target ^ (Target >> 16U)) * Inverse) - hash;
    EncodeFixed32(MutableByteView(key).last<4>(), final_word);
    return key;
}

static_assert(!std::is_copy_constructible_v<ClockBlockCache::Handle>);
static_assert(std::is_nothrow_move_constructible_v<ClockBlockCache::Handle>);
static_assert(std::is_nothrow_move_assignable_v<ClockBlockCache::Handle>);

TEST(ClockBlockCacheTest, LookupRetainsTheExactImmutableBlockWithoutTransferringOwnership) {
    ClockBlockCache cache(16 * 4096);
    const auto key = KeyOf(11, 37);
    auto inserted = cache.Insert(key, ValueOf("value"), 4096);
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(TextOf(*inserted), "value");
    const auto found = cache.Lookup(key);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(&found->value(), &inserted->value());
    EXPECT_EQ(TextOf(*found), "value");
    EXPECT_EQ(cache.total_charge(), 4096U);
    EXPECT_FALSE(cache.Lookup(KeyOf(11, 38)).has_value());
    EXPECT_FALSE(cache.Lookup(KeyOf(12, 37)).has_value());
}

TEST(ClockBlockCacheTest, EraseAndReplacementKeepEveryOldLeaseAlive) {
    ClockBlockCache cache(16 * 4096);
    const auto key = KeyOf(1, 9);
    auto old = cache.Insert(key, ValueOf("old"), 4096);
    ASSERT_TRUE(old.has_value());
    auto replacement = cache.Insert(key, ValueOf("new"), 4096);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_EQ(TextOf(*old), "old");
    EXPECT_EQ(TextOf(*replacement), "new");
    const auto found = cache.Lookup(key);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(TextOf(*found), "new");
    EXPECT_EQ(cache.total_charge(), 4096U);
    cache.Erase(key);
    EXPECT_FALSE(cache.Lookup(key).has_value());
    EXPECT_EQ(cache.total_charge(), 0U);
    EXPECT_EQ(TextOf(*old), "old");
    EXPECT_EQ(TextOf(*replacement), "new");
    EXPECT_EQ(TextOf(*found), "new");
}

TEST(ClockBlockCacheTest, ZeroCapacityReturnsAnUncachedLeaseAndReleasesExternalAccounting) {
    ClockBlockCache cache(0);
    const auto key = KeyOf(7, 8);
    {
        auto inserted = cache.Insert(key, ValueOf("uncached"), 4096);
        ASSERT_TRUE(inserted.has_value());
        EXPECT_EQ(TextOf(*inserted), "uncached");
        EXPECT_FALSE(cache.Lookup(key).has_value());
        EXPECT_EQ(cache.total_charge(), 0U);
        EXPECT_GT(cache.GetStats().standalone_bytes, 0U);
    }
    EXPECT_EQ(cache.GetStats().standalone_bytes, 0U);
    EXPECT_GT(cache.GetStats().peak_standalone_bytes, 0U);
}

TEST(ClockBlockCacheTest, UncachedReplacementInvalidatesTheOldMappingButNotItsLease) {
    ClockBlockCache cache(16 * 4096);
    const auto key = KeyOf(3, 4);
    auto old = cache.Insert(key, ValueOf("old"), 4096);
    ASSERT_TRUE(old.has_value());
    auto oversized =
        cache.Insert(key, ValueOf("standalone"), std::numeric_limits<std::size_t>::max());
    ASSERT_TRUE(oversized.has_value());
    EXPECT_EQ(TextOf(*old), "old");
    EXPECT_EQ(TextOf(*oversized), "standalone");
    EXPECT_FALSE(cache.Lookup(key).has_value());
    EXPECT_EQ(cache.total_charge(), 0U);
}

TEST(ClockBlockCacheTest, RejectsInvalidOwnersAndChargesWithoutChangingAnExistingMapping) {
    ClockBlockCache cache(16 * 4096);
    const auto key = KeyOf(9, 10);
    ASSERT_TRUE(cache.Insert(key, ValueOf("old"), 4096).has_value());
    const auto null = cache.Insert(key, nullptr, 4096);
    ASSERT_FALSE(null.has_value());
    EXPECT_EQ(null.error().code(), ErrorCode::InvalidArgument);
    const auto free = cache.Insert(key, ValueOf("wrong"), 0);
    ASSERT_FALSE(free.has_value());
    EXPECT_EQ(free.error().code(), ErrorCode::InvalidArgument);
    const auto found = cache.Lookup(key);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(TextOf(*found), "old");
    EXPECT_EQ(cache.total_charge(), 4096U);
}

TEST(ClockBlockCacheTest, FullKeysRemainReachableAcrossExactHashClustersAndIndexGrowth) {
    ClockBlockCache cache(16 * 4096 * 128);
    for (std::uint32_t index = 0; index < 96; ++index) {
        const auto key = CollidingKey(index);
        ASSERT_EQ(Hash32(key, 0), 0x01234567U);
        ASSERT_TRUE(cache.Insert(key, ValueOf(std::to_string(index)), 4096).has_value());
    }
    EXPECT_EQ(cache.total_charge(), 96U * 4096U);
    for (std::uint32_t index = 0; index < 96; ++index) {
        const auto found = cache.Lookup(CollidingKey(index));
        ASSERT_TRUE(found.has_value()) << index;
        EXPECT_EQ(TextOf(*found), std::to_string(index));
    }
    const auto stats = cache.GetStats();
    EXPECT_GT(stats.retained_index_bytes, 0U);
    EXPECT_LE(stats.metadata_bytes, stats.metadata_allowance);
}

TEST(ClockBlockCacheTest, RefreshAndPositionReuseDoNotAccumulateTombstoneSnapshots) {
    ClockBlockCache cache(16 * 4096 * 8);
    auto key = CollidingKey(0);
    ASSERT_TRUE(cache.Insert(key, ValueOf("value"), 4096).has_value());
    const auto initial = cache.GetStats();
    for (std::uint32_t index = 1; index <= 1024; ++index) {
        cache.Erase(key);
        key = CollidingKey(index);
        ASSERT_TRUE(cache.Insert(key, ValueOf("value"), 4096).has_value());
        ASSERT_TRUE(cache.Lookup(key).has_value());
    }
    const auto after = cache.GetStats();
    EXPECT_EQ(after.metadata_bytes, initial.metadata_bytes);
    EXPECT_EQ(after.retained_index_bytes, initial.retained_index_bytes);
    EXPECT_LE(after.metadata_bytes, after.metadata_allowance);
    EXPECT_EQ(cache.total_charge(), 4096U);
}

TEST(ClockBlockCacheTest, MoveAndAllPinnedPressurePreserveOwnershipWithoutUnboundedRetention) {
    ClockBlockCache cache(16 * 4096);
    auto first = cache.Insert(CollidingKey(1), ValueOf("first"), 4096);
    ASSERT_TRUE(first.has_value());
    ClockBlockCache::Handle moved(std::move(*first));
    auto second = cache.Insert(CollidingKey(2), ValueOf("second"), 4096);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(TextOf(moved), "first");
    EXPECT_EQ(TextOf(*second), "second");
    EXPECT_EQ(cache.total_charge(), 4096U);
    EXPECT_FALSE(cache.Lookup(CollidingKey(2)).has_value());
    const auto stats = cache.GetStats();
    EXPECT_GT(stats.admission_rejections, 0U);
    EXPECT_LE(stats.metadata_bytes, stats.metadata_allowance);
}

}  // namespace
}  // namespace modern_leveldb
