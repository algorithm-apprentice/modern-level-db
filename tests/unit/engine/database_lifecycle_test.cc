#include <type_traits>

#include "database_test_support.h"

namespace modern_leveldb {
namespace database_test {

static_assert(std::is_aggregate_v<DatabaseEngineOptions>);
static_assert(std::is_same_v<decltype(&DatabaseEngineOptions::Sanitize),
                             void (DatabaseEngineOptions::*)() noexcept>);

TEST(DatabaseOptionsTest, ClipsOptionsToLevelDbsRanges) {
    DatabaseEngineOptions small;
    small.max_open_files = 1;
    small.memtable_size_threshold = 1;
    small.max_file_size = 1;
    small.table_options.block_size = 1;
    small.Sanitize();
    EXPECT_EQ(small.max_open_files, 74U);
    EXPECT_EQ(small.memtable_size_threshold, std::size_t{64} << 10U);
    EXPECT_EQ(small.max_file_size, std::uint64_t{1} << 20U);
    EXPECT_EQ(small.table_options.block_size, std::size_t{1} << 10U);

    DatabaseEngineOptions large;
    large.max_open_files = 1000000;
    large.memtable_size_threshold = std::size_t{1} << 40U;
    large.max_file_size = std::uint64_t{1} << 40U;
    large.table_options.block_size = std::size_t{1} << 30U;
    large.Sanitize();
    EXPECT_EQ(large.max_open_files, 50000U);
    EXPECT_EQ(large.memtable_size_threshold, std::size_t{1} << 30U);
    EXPECT_EQ(large.max_file_size, std::uint64_t{1} << 30U);
    EXPECT_EQ(large.table_options.block_size, std::size_t{4} << 20U);

    DatabaseEngineOptions defaults;
    defaults.Sanitize();
    EXPECT_TRUE(defaults.sync_wal_creation);
    EXPECT_EQ(defaults.max_open_files, 1000U);
    EXPECT_EQ(defaults.memtable_size_threshold, std::size_t{4} << 20U);
    EXPECT_EQ(defaults.max_file_size, std::uint64_t{2} << 20U);
    EXPECT_EQ(defaults.table_options.block_size, std::size_t{4} << 10U);
}

TEST(DatabaseOptionsTest, PreservesUnrelatedOptionsAndIsIdempotent) {
    BlockingComparator comparator;
    BlockCache cache(1024);
    MemoryFileSystem file_system;
    ManualExecutor executor;
    ManualClock clock;
    DatabaseEngineOptions options;
    options.comparator = &comparator;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.sync_wal_creation = false;
    options.memtable_size_threshold = 1;
    options.max_file_size = std::uint64_t{1} << 40U;
    options.max_open_files = 1;
    options.allow_mmap_reads = false;
    options.table_options.block_size = std::size_t{1} << 30U;
    options.table_options.restart_interval = 23;
    options.table_options.filter_policy.emplace(11);
    options.table_options.compression = BlockCompression::Zstd;
    options.table_options.zstd_compression_level = 3;
    options.block_cache = &cache;
    options.file_system = &file_system;
    options.executor = &executor;
    options.clock = &clock;
    options.allow_weak_namespace_durability = true;
    const auto filter_size = options.table_options.filter_policy->FilterSize(128);

    for (int repeat = 0; repeat < 2; ++repeat) {
        SCOPED_TRACE(repeat);
        options.Sanitize();

        EXPECT_EQ(options.max_open_files, 74U);
        EXPECT_EQ(options.memtable_size_threshold, std::size_t{64} << 10U);
        EXPECT_EQ(options.max_file_size, std::uint64_t{1} << 30U);
        EXPECT_EQ(options.table_options.block_size, std::size_t{4} << 20U);
        EXPECT_EQ(options.comparator, &comparator);
        EXPECT_TRUE(options.create_if_missing);
        EXPECT_TRUE(options.error_if_exists);
        EXPECT_FALSE(options.sync_wal_creation);
        EXPECT_FALSE(options.allow_mmap_reads);
        EXPECT_EQ(options.table_options.restart_interval, 23U);
        ASSERT_TRUE(options.table_options.filter_policy.has_value());
        EXPECT_EQ(options.table_options.filter_policy->FilterSize(128), filter_size);
        EXPECT_EQ(options.table_options.compression, BlockCompression::Zstd);
        EXPECT_EQ(options.table_options.zstd_compression_level, 3);
        EXPECT_EQ(options.block_cache, &cache);
        EXPECT_EQ(options.file_system, &file_system);
        EXPECT_EQ(options.executor, &executor);
        EXPECT_EQ(options.clock, &clock);
        EXPECT_TRUE(options.allow_weak_namespace_durability);
    }
}

TEST_F(DatabaseTest, SanitizesACopyOfTheCallerOptions) {
    options_.max_open_files = 1;
    options_.max_file_size = 1;
    options_.table_options.block_size = 1;

    const auto database = Open();
    ASSERT_NE(database, nullptr);

    EXPECT_EQ(options_.max_open_files, 1U);
    EXPECT_EQ(options_.memtable_size_threshold, 1U);
    EXPECT_EQ(options_.max_file_size, 1U);
    EXPECT_EQ(options_.table_options.block_size, 1U);
}

TEST_F(DatabaseTest, OpensNewAndExistingDatabases) {
    options_.create_if_missing = false;
    const auto missing = TryOpen();
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);

    options_.create_if_missing = true;
    {
        const auto database = Open();
        ASSERT_TRUE(Put(*database, "a", "1").has_value());
        // A second database cannot lock the directory.
        const auto locked = TryOpen();
        ASSERT_FALSE(locked.has_value());
        EXPECT_EQ(locked.error().code(), ErrorCode::Busy);
    }

    // Writes that only the log holds are recovered.
    options_.create_if_missing = false;
    {
        const auto database = Open();
        EXPECT_EQ(Get(*database, "a"), "1");
    }
    options_.error_if_exists = true;
    const auto exists = TryOpen();
    ASSERT_FALSE(exists.has_value());
    EXPECT_EQ(exists.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(DatabaseTest, RemovesObsoleteFilesWhenItOpens) {
    {
        const auto database = Open();
    }
    const std::uint64_t manifest = Numbers(FileType::Descriptor).back();
    file_system_.Write(directory_ / Numbered(1, ".log"), {});
    file_system_.Write(directory_ / Numbered(900, ".ldb"), {});
    file_system_.Write(directory_ / Numbered(901, ".dbtmp"), {});
    file_system_.Write(directory_ / "LOG", {});

    const auto database = Open();

    EXPECT_FALSE(Exists(Numbered(1, ".log")));
    EXPECT_FALSE(Exists(Numbered(900, ".ldb")));
    EXPECT_FALSE(Exists(Numbered(901, ".dbtmp")));
    EXPECT_TRUE(Exists("LOG"));
    EXPECT_TRUE(Exists("CURRENT"));
    EXPECT_TRUE(Exists("LOCK"));
    // Reopening wrote a new MANIFEST, so the old one is removed.
    EXPECT_FALSE(Exists("MANIFEST-" + Numbered(manifest, "")));
    EXPECT_EQ(Numbers(FileType::Descriptor).size(), 1U);
}

TEST_F(DatabaseTest, OpensWhenCleanupCannotListTheDirectory) {
    {
        const auto database = Open();
    }
    file_system_.Write(directory_ / Numbered(900, ".ldb"), {});
    // Recovery lists the directory first, and then cleanup does.
    int lists = 0;
    file_system_.SetOperationHook([&](std::string_view operation) -> Status {
        if (operation == "list db" && ++lists == 2) {
            return std::unexpected(Error::Io("injected failure"));
        }
        return {};
    });

    const auto database = Open();

    file_system_.SetOperationHook({});
    EXPECT_EQ(lists, 2);
    EXPECT_TRUE(Exists(Numbered(900, ".ldb")));
}

TEST_F(DatabaseTest, IgnoresAnExceptionWhileClosingTheCurrentLog) {
    EXPECT_EXIT(
        {
            MemoryFileSystem file_system;
            ManualExecutor executor;
            ManualClock clock;
            DatabaseEngineOptions options;
            options.file_system = &file_system;
            options.executor = &executor;
            options.clock = &clock;
            options.create_if_missing = true;
            options.memtable_size_threshold = 1;
            Result<std::unique_ptr<DatabaseEngine>> opened =
                DatabaseEngine::Open(options, "database");
            if (!opened.has_value()) {
                std::exit(1);
            }
            std::unique_ptr<DatabaseEngine> database = std::move(*opened);
            file_system.SetOperationHook([](std::string_view operation) -> Status {
                if (operation.starts_with("close ") && operation.ends_with(".log")) {
                    throw std::runtime_error("thrown");
                }
                return {};
            });

            database.reset();
            std::exit(0);
        },
        testing::ExitedWithCode(0), "");
}

}  // namespace database_test
}  // namespace modern_leveldb
