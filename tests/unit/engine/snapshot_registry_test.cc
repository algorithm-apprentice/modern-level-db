#include "engine/snapshot_registry.h"

#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

namespace modern_leveldb {
namespace {

static_assert(!std::is_copy_constructible_v<SnapshotRegistry::Registration>);
static_assert(std::is_nothrow_move_constructible_v<SnapshotRegistry::Registration>);

TEST(SnapshotRegistryTest, EmptyRegistryUsesTheFallback) {
    const SnapshotRegistry registry;

    EXPECT_EQ(registry.size(), 0U);
    EXPECT_FALSE(registry.oldest().has_value());
    EXPECT_EQ(registry.OldestOr(17), 17U);
}

TEST(SnapshotRegistryTest, TracksDuplicatesAndTheOldestSequence) {
    SnapshotRegistry registry;
    auto first = registry.Register(3);
    auto duplicate = registry.Register(3);
    auto newest = registry.Register(7);

    ASSERT_TRUE(registry.oldest().has_value());
    EXPECT_EQ(*registry.oldest(), 3U);
    EXPECT_EQ(first.sequence(), 3U);
    EXPECT_EQ(duplicate.sequence(), 3U);
    EXPECT_EQ(newest.sequence(), 7U);
    EXPECT_EQ(registry.OldestOr(99), 3U);
    EXPECT_EQ(registry.size(), 3U);

    registry.Release(first);
    ASSERT_TRUE(registry.oldest().has_value());
    EXPECT_EQ(*registry.oldest(), 3U);
    EXPECT_EQ(registry.size(), 2U);

    registry.Release(duplicate);
    ASSERT_TRUE(registry.oldest().has_value());
    EXPECT_EQ(*registry.oldest(), 7U);

    registry.Release(newest);
    EXPECT_FALSE(registry.oldest().has_value());
    EXPECT_EQ(registry.OldestOr(99), 99U);
}

TEST(SnapshotRegistryTest, ReleasedRegistrationsRemainSafelyMovableValues) {
    SnapshotRegistry registry;
    auto registration = registry.Register(11);
    registry.Release(registration);

    auto moved = std::move(registration);
    EXPECT_EQ(moved.sequence(), 11U);
    EXPECT_EQ(registry.size(), 0U);
}

TEST(SnapshotRegistryTest, UnlinksMiddleAndNewestRegistrations) {
    SnapshotRegistry registry;
    auto oldest = registry.Register(1);
    auto middle = registry.Register(2);
    auto newest = registry.Register(3);

    auto moved_middle = std::move(middle);
    registry.Release(moved_middle);
    EXPECT_EQ(registry.size(), 2U);
    EXPECT_EQ(registry.OldestOr(99), 1U);

    registry.Release(newest);
    EXPECT_EQ(registry.size(), 1U);
    EXPECT_EQ(registry.OldestOr(99), 1U);

    registry.Release(oldest);
    EXPECT_EQ(registry.size(), 0U);
}

}  // namespace
}  // namespace modern_leveldb
