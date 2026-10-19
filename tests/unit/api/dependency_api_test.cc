#include <gtest/gtest.h>

#include <memory>
#include <type_traits>

#include "modern_leveldb/background_executor.h"
#include "modern_leveldb/block_cache.h"
#include "modern_leveldb/clock.h"
#include "modern_leveldb/file_system.h"

namespace modern_leveldb {
namespace {

static_assert(std::is_abstract_v<FileSystem>);
static_assert(std::is_abstract_v<Clock>);
static_assert(std::is_abstract_v<BackgroundExecutor>);
static_assert(std::has_virtual_destructor_v<FileSystem>);
static_assert(std::has_virtual_destructor_v<Clock>);
static_assert(std::has_virtual_destructor_v<BackgroundExecutor>);
static_assert(!std::is_copy_constructible_v<FileSystem>);
static_assert(!std::is_copy_constructible_v<Clock>);
static_assert(!std::is_copy_constructible_v<BackgroundExecutor>);

TEST(PublicDependencyTest, CreatesAnOpaqueBlockCache) {
    const auto cache = CreateBlockCache(4096);
    ASSERT_NE(cache, nullptr);
    EXPECT_EQ(BlockCacheCharge(*cache), 0U);
}

TEST(PublicDependencyTest, SupportsZeroCapacityAndIndependentSharedOwners) {
    auto cache = CreateBlockCache(0);
    ASSERT_NE(cache, nullptr);
    EXPECT_EQ(BlockCacheCharge(*cache), 0U);
    const std::weak_ptr<BlockCache> lifetime = cache;
    auto retained = cache;
    cache.reset();
    EXPECT_FALSE(lifetime.expired());
    retained.reset();
    EXPECT_TRUE(lifetime.expired());
}

}  // namespace
}  // namespace modern_leveldb
