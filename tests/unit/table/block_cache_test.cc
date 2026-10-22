#include "table/block_cache.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "table/block_builder.h"
#include "table/clock_block_cache.h"

namespace modern_leveldb {
namespace {

using CacheKey = std::array<std::byte, 16>;

CacheKey KeyOf(std::uint64_t file, std::uint64_t offset) {
    CacheKey key{};
    EncodeFixed64(MutableByteView(key).first<8>(), file);
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

std::string TextOf(const BlockCache::Handle& handle) {
    Block::Iterator cursor(handle.value(), BytewiseComparator());
    cursor.SeekToFirst().value();
    return std::string(AsStringView(cursor.value()));
}

static_assert(!std::is_copy_constructible_v<BlockCache::Handle>);
static_assert(std::is_nothrow_move_constructible_v<BlockCache::Handle>);
static_assert(std::is_nothrow_move_assignable_v<BlockCache::Handle>);

TEST(BlockCacheFacadeTest, BothBackendsRetainExactImmutableBlocksAndCompleteNamespaces) {
    for (const auto policy : {BlockCachePolicy::Lru, BlockCachePolicy::Clock}) {
        SCOPED_TRACE(static_cast<int>(policy));
        BlockCache cache(16 * 4096 * 4, policy);
        const auto key = KeyOf(1, 17);
        auto inserted = cache.Insert(key, ValueOf("value"), 4096);
        ASSERT_TRUE(inserted.has_value());
        const auto found = cache.Lookup(key);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(&found->value(), &inserted->value());
        EXPECT_EQ(TextOf(*found), "value");
        EXPECT_FALSE(cache.Lookup(KeyOf(1, 18)).has_value());
        EXPECT_FALSE(cache.Lookup(KeyOf(2, 17)).has_value());
        EXPECT_EQ(cache.total_charge(), 4096U);
        const auto first = cache.NewId();
        EXPECT_NE(first, cache.NewId());
    }
}

TEST(BlockCacheFacadeTest, ReplacementPreservesOldLeasesInBothBackends) {
    for (const auto policy : {BlockCachePolicy::Lru, BlockCachePolicy::Clock}) {
        SCOPED_TRACE(static_cast<int>(policy));
        BlockCache cache(16 * 4096 * 4, policy);
        const auto key = KeyOf(1, 17);
        auto old = cache.Insert(key, ValueOf("old"), 4096);
        ASSERT_TRUE(old.has_value());
        auto replacement = cache.Insert(key, ValueOf("new"), 4096);
        ASSERT_TRUE(replacement.has_value());
        const auto current = cache.Lookup(key);
        ASSERT_TRUE(current.has_value());
        EXPECT_EQ(TextOf(*old), "old");
        EXPECT_EQ(TextOf(*replacement), "new");
        EXPECT_EQ(TextOf(*current), "new");
        EXPECT_EQ(cache.total_charge(), 4096U);
    }
}

TEST(BlockCacheFacadeTest, CrossBackendMoveAssignmentUsesTheLeasesOwnTypedAlternative) {
    BlockCache lru(16 * 4096, BlockCachePolicy::Lru);
    BlockCache clock(16 * 4096, BlockCachePolicy::Clock);
    auto first = lru.Insert(KeyOf(1, 1), ValueOf("lru"), 4096).value();
    auto second = clock.Insert(KeyOf(2, 2), ValueOf("clock"), 4096).value();
    const auto* clock_value = &second.value();
    first = std::move(second);
    EXPECT_EQ(&first.value(), clock_value);
    EXPECT_EQ(TextOf(first), "clock");
    auto third = lru.Insert(KeyOf(3, 3), ValueOf("other-lru"), 4096).value();
    const auto* lru_value = &third.value();
    second = std::move(third);
    EXPECT_EQ(&second.value(), lru_value);
    EXPECT_EQ(TextOf(second), "other-lru");
    auto* same = &first;
    first = std::move(*same);
    EXPECT_EQ(TextOf(first), "clock");
}

TEST(BlockCacheFacadeTest, DefaultLruKeepsArbitraryFullKeySemantics) {
    BlockCache cache(16 * 4096);
    const std::array<std::string, 4> keys{"", "prefix", std::string("prefix\0", 7),
                                          std::string(37, '\xff')};
    for (std::size_t index = 0; index < keys.size(); ++index) {
        ASSERT_TRUE(
            cache.Insert(AsBytes(keys[index]), ValueOf(std::to_string(index)), 1).has_value());
    }
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const auto found = cache.Lookup(AsBytes(keys[index]));
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(TextOf(*found), std::to_string(index));
    }
    EXPECT_FALSE(cache.GetClockStats().has_value());
}

TEST(BlockCacheFacadeTest, ZeroCapacityReturnsLiveUncachedLeasesForEitherPolicy) {
    for (const auto policy : {BlockCachePolicy::Lru, BlockCachePolicy::Clock}) {
        SCOPED_TRACE(static_cast<int>(policy));
        BlockCache cache(0, policy);
        {
            auto inserted = cache.Insert(KeyOf(1, 1), ValueOf("uncached"), 4096);
            ASSERT_TRUE(inserted.has_value());
            EXPECT_EQ(TextOf(*inserted), "uncached");
            EXPECT_FALSE(cache.Lookup(KeyOf(1, 1)).has_value());
            EXPECT_EQ(cache.total_charge(), 0U);
            if (policy == BlockCachePolicy::Clock) {
                const auto stats = cache.GetClockStats();
                ASSERT_TRUE(stats.has_value());
                EXPECT_GT(stats->standalone_bytes, 0U);
            }
        }
        if (policy == BlockCachePolicy::Clock) {
            const auto stats = cache.GetClockStats();
            ASSERT_TRUE(stats.has_value());
            EXPECT_EQ(stats->standalone_bytes, 0U);
        }
    }
}

TEST(BlockCacheFacadeTest, OversizedClockReplacementInvalidatesTheOldMappingButKeepsBothLeases) {
    BlockCache cache(16 * 4096, BlockCachePolicy::Clock);
    const auto key = KeyOf(1, 1);
    auto old = cache.Insert(key, ValueOf("old"), 4096);
    ASSERT_TRUE(old.has_value());
    auto standalone =
        cache.Insert(key, ValueOf("standalone"), std::numeric_limits<std::size_t>::max());
    ASSERT_TRUE(standalone.has_value());
    EXPECT_EQ(TextOf(*old), "old");
    EXPECT_EQ(TextOf(*standalone), "standalone");
    EXPECT_FALSE(cache.Lookup(key).has_value());
    EXPECT_EQ(cache.total_charge(), 0U);
}

TEST(BlockCacheFacadeTest, LruPreservesItsLegacyPinnedOversizeRetention) {
    BlockCache cache(16 * 4096, BlockCachePolicy::Lru);
    const auto key = KeyOf(1, 1);
    auto oversized = cache.Insert(key, ValueOf("oversized"), 4097);
    ASSERT_TRUE(oversized.has_value());
    const auto found = cache.Lookup(key);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(TextOf(*found), "oversized");
    EXPECT_EQ(cache.total_charge(), 4097U);
}

TEST(BlockCacheFacadeTest, InvalidOwnersAndChargesDoNotChangeEitherCurrentMapping) {
    for (const auto policy : {BlockCachePolicy::Lru, BlockCachePolicy::Clock}) {
        SCOPED_TRACE(static_cast<int>(policy));
        BlockCache cache(16 * 4096, policy);
        const auto key = KeyOf(1, 1);
        ASSERT_TRUE(cache.Insert(key, ValueOf("current"), 4096).has_value());
        const auto null = cache.Insert(key, nullptr, 4096);
        ASSERT_FALSE(null.has_value());
        EXPECT_EQ(null.error().code(), ErrorCode::InvalidArgument);
        const auto zero = cache.Insert(key, ValueOf("wrong"), 0);
        ASSERT_FALSE(zero.has_value());
        EXPECT_EQ(zero.error().code(), ErrorCode::InvalidArgument);
        const auto found = cache.Lookup(key);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(TextOf(*found), "current");
    }
}

TEST(BlockCacheFacadeTest, CheckedFactoryAndAutomaticSelectTheIntendedTypedBackend) {
    for (const auto policy :
         {BlockCachePolicy::Automatic, BlockCachePolicy::Lru, BlockCachePolicy::Clock}) {
        const auto created = CreateBlockCacheWithOptions({.capacity = 16 * 4096, .policy = policy});
        ASSERT_TRUE(created.has_value());
        EXPECT_EQ((*created)->GetClockStats().has_value(), policy == BlockCachePolicy::Clock);
    }
}

TEST(BlockCacheFacadeTest, ClockStatsAccountForTheCompleteOwningVariantNotJustItsMember) {
    constexpr std::size_t Capacity = 16 * 4096;
    ClockBlockCache standalone(Capacity);
    BlockCache facade(Capacity, BlockCachePolicy::Clock);
    const auto base = standalone.GetStats();
    const auto wrapped = facade.GetClockStats();
    ASSERT_TRUE(wrapped.has_value());
    constexpr auto Extra = sizeof(BlockCache) - sizeof(ClockBlockCache);
    ASSERT_GT(Extra, 15U);
    EXPECT_GE(wrapped->metadata_bytes, base.metadata_bytes + Extra - 15);
    EXPECT_LE(wrapped->metadata_bytes, base.metadata_bytes + Extra + 15);
    EXPECT_EQ(wrapped->metadata_allowance, base.metadata_allowance);
    EXPECT_LE(wrapped->metadata_bytes, wrapped->metadata_allowance);
}

TEST(BlockCacheFacadeTest, AnExplicitOwnerFootprintReplacesTheStandaloneObjectAccount) {
    constexpr std::size_t Capacity = 16 * 4096;
    ClockBlockCache standalone(Capacity);
    ClockBlockCache wrapped(Capacity,
                            clock_cache_detail::OwnerFootprint{sizeof(ClockBlockCache) + 4096});
    const auto base = standalone.GetStats();
    const auto full = wrapped.GetStats();
    EXPECT_EQ(full.metadata_bytes, base.metadata_bytes + 4096);
    EXPECT_EQ(full.metadata_allowance, base.metadata_allowance);
}

TEST(BlockCacheFacadeTest, FacadeClockMetadataBudgetStillBoundsTinyChargedAdmissions) {
    BlockCache cache(16 * 4096, BlockCachePolicy::Clock);
    for (std::uint64_t index = 0; index < 1024; ++index) {
        auto inserted = cache.Insert(KeyOf(17, index), ValueOf("tiny"), 1);
        ASSERT_TRUE(inserted.has_value());
        EXPECT_EQ(TextOf(*inserted), "tiny");
    }
    const auto stats = cache.GetClockStats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_GT(stats->admission_rejections, 0U);
    EXPECT_LE(stats->metadata_bytes, stats->metadata_allowance);
    EXPECT_EQ(stats->standalone_bytes, 0U);
}

}  // namespace
}  // namespace modern_leveldb
