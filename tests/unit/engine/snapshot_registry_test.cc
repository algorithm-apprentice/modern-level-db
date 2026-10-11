#include "engine/snapshot_registry.h"

#include <gtest/gtest.h>

namespace modern_leveldb {
namespace {

TEST(SnapshotRegistryTest, EmptyRegistryUsesTheFallback) {
    const SnapshotRegistry registry;

    EXPECT_EQ(registry.size(), 0U);
    EXPECT_FALSE(registry.oldest().has_value());
    EXPECT_EQ(registry.OldestOr(17), 17U);
}

TEST(SnapshotRegistryTest, TracksDuplicatesAndTheOldestSequence) {
    SnapshotRegistry registry;
    registry.Register(7);
    registry.Register(3);
    registry.Register(3);

    ASSERT_TRUE(registry.oldest().has_value());
    EXPECT_EQ(*registry.oldest(), 3U);
    EXPECT_EQ(registry.OldestOr(99), 3U);
    EXPECT_EQ(registry.size(), 3U);

    registry.Release(3);
    ASSERT_TRUE(registry.oldest().has_value());
    EXPECT_EQ(*registry.oldest(), 3U);
    EXPECT_EQ(registry.size(), 2U);

    registry.Release(3);
    ASSERT_TRUE(registry.oldest().has_value());
    EXPECT_EQ(*registry.oldest(), 7U);

    registry.Release(7);
    EXPECT_FALSE(registry.oldest().has_value());
    EXPECT_EQ(registry.OldestOr(99), 99U);
}

}  // namespace
}  // namespace modern_leveldb
