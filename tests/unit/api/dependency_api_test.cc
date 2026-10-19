#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "dependency_test_support.h"
#include "modern_leveldb/background_executor.h"
#include "modern_leveldb/block_cache.h"
#include "modern_leveldb/clock.h"
#include "modern_leveldb/db.h"
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
static_assert(std::is_copy_constructible_v<Options>);
static_assert(std::is_aggregate_v<Options>);
static_assert(std::is_nothrow_move_constructible_v<Options>);

TEST(PublicDependencyTest, DefaultsToInternallyCreatedServices) {
    const Options options;
    EXPECT_EQ(options.file_system, nullptr);
    EXPECT_EQ(options.block_cache, nullptr);
    EXPECT_EQ(options.clock, nullptr);
    EXPECT_EQ(options.executor, nullptr);
}

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

TEST(PublicDependencyTest, PropagatesAnInjectedFilesystemFailure) {
    auto filesystem = std::make_shared<dependency_test_support::FailingFileSystem>();
    Options options;
    options.create_if_missing = true;
    options.file_system = filesystem;

    const auto opened = Database::Open(options, "injected-database");

    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code(), ErrorCode::Io);
    EXPECT_EQ(opened.error().message(), "injected filesystem failure");
    EXPECT_GT(filesystem->call_count(), 0);
}

TEST(PublicDependencyTest, RejectsInvalidOptionsBeforeCallingAnInjectedFilesystem) {
    auto filesystem = std::make_shared<dependency_test_support::FailingFileSystem>();
    Options options;
    options.block_restart_interval = 0;
    options.file_system = filesystem;

    const auto opened = Database::Open(options, "injected-database");

    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(filesystem->call_count(), 0);
}

TEST(PublicDependencyTest, ReleasesRetainedServicesAfterOpenFailureOrException) {
    for (bool throws : {false, true}) {
        SCOPED_TRACE(throws);
        Options options;
        options.create_if_missing = true;
        options.file_system = std::make_shared<dependency_test_support::FailingFileSystem>(throws);
        options.block_cache = CreateBlockCache(4096);
        options.clock = std::make_shared<dependency_test_support::CountingClock>();
        options.executor = std::make_shared<dependency_test_support::QueuedExecutor>();
        const std::array<std::weak_ptr<void>, 4> lifetimes{options.file_system, options.block_cache,
                                                           options.clock, options.executor};

        if (throws) {
            EXPECT_THROW(static_cast<void>(Database::Open(std::move(options), "database")),
                         std::runtime_error);
        } else {
            const auto opened = Database::Open(std::move(options), "database");
            ASSERT_FALSE(opened.has_value());
            EXPECT_EQ(opened.error().code(), ErrorCode::Io);
        }

        for (const auto& lifetime : lifetimes) {
            EXPECT_TRUE(lifetime.expired());
        }
    }
}

}  // namespace
}  // namespace modern_leveldb
