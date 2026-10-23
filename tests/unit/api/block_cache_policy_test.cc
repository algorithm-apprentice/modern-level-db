#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <type_traits>

#include "modern_leveldb/base/result.h"
#include "modern_leveldb/block_cache.h"

namespace modern_leveldb {
namespace {

using LegacyCacheFactory = std::shared_ptr<BlockCache> (*)(std::size_t);
using CheckedCacheFactory = Result<std::shared_ptr<BlockCache>> (*)(const BlockCacheOptions&);
static_assert(std::is_same_v<decltype(&CreateBlockCache), LegacyCacheFactory>);
static_assert(std::is_same_v<decltype(&CreateBlockCacheWithOptions), CheckedCacheFactory>);

TEST(PublicBlockCachePolicyTest, DefaultsToAutomaticAndZeroExplicitCapacity) {
    const BlockCacheOptions options;
    EXPECT_EQ(options.capacity, 0U);
    EXPECT_EQ(options.policy, BlockCachePolicy::Automatic);
}

TEST(PublicBlockCachePolicyTest, LegacyBraceAndFunctionAddressCallsRemainUnambiguous) {
    const auto zero = CreateBlockCache({});
    ASSERT_NE(zero, nullptr);
    EXPECT_EQ(BlockCacheCharge(*zero), 0U);
    LegacyCacheFactory legacy = &CreateBlockCache;
    const auto ordinary = legacy(4096);
    ASSERT_NE(ordinary, nullptr);
    EXPECT_EQ(BlockCacheCharge(*ordinary), 0U);
}

TEST(PublicBlockCachePolicyTest, CheckedFactoryAcceptsEveryNamedPolicy) {
    for (const auto policy :
         {BlockCachePolicy::Automatic, BlockCachePolicy::Lru, BlockCachePolicy::Clock}) {
        const auto cache = CreateBlockCacheWithOptions({.capacity = 16 * 4096, .policy = policy});
        ASSERT_TRUE(cache.has_value()) << cache.error().ToString();
        ASSERT_NE(*cache, nullptr);
        EXPECT_EQ(BlockCacheCharge(**cache), 0U);
    }
}

TEST(PublicBlockCachePolicyTest, InvalidPolicyIsAnExplicitErrorRatherThanAnAutomaticFallback) {
    for (const auto invalid : {-1, 3, 255}) {
        const auto cache = CreateBlockCacheWithOptions(
            {.capacity = 16 * 4096, .policy = static_cast<BlockCachePolicy>(invalid)});
        ASSERT_FALSE(cache.has_value());
        EXPECT_EQ(cache.error().code(), ErrorCode::InvalidArgument);
    }
}

}  // namespace
}  // namespace modern_leveldb
