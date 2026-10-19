#include "database_test_support.h"

namespace modern_leveldb {
namespace database_test {

TEST(DatabaseOptionsTest, ClipsOptionsToLevelDbsRanges) {
    DatabaseEngineOptions small;
    small.max_open_files = 1;
    small.memtable_size_threshold = 1;
    small.max_file_size = 1;
    small.table_options.block_size = 1;
    const DatabaseEngineOptions low = SanitizeOptions(small);
    EXPECT_EQ(low.max_open_files, 74U);
    EXPECT_EQ(low.memtable_size_threshold, std::size_t{64} << 10U);
    EXPECT_EQ(low.max_file_size, std::uint64_t{1} << 20U);
    EXPECT_EQ(low.table_options.block_size, std::size_t{1} << 10U);

    DatabaseEngineOptions large;
    large.max_open_files = 1000000;
    large.memtable_size_threshold = std::size_t{1} << 40U;
    large.max_file_size = std::uint64_t{1} << 40U;
    large.table_options.block_size = std::size_t{1} << 30U;
    const DatabaseEngineOptions high = SanitizeOptions(large);
    EXPECT_EQ(high.max_open_files, 50000U);
    EXPECT_EQ(high.memtable_size_threshold, std::size_t{1} << 30U);
    EXPECT_EQ(high.max_file_size, std::uint64_t{1} << 30U);
    EXPECT_EQ(high.table_options.block_size, std::size_t{4} << 20U);

    const DatabaseEngineOptions defaults = SanitizeOptions(DatabaseEngineOptions());
    EXPECT_TRUE(defaults.sync_wal_creation);
    EXPECT_EQ(defaults.max_open_files, 1000U);
    EXPECT_EQ(defaults.memtable_size_threshold, std::size_t{4} << 20U);
    EXPECT_EQ(defaults.max_file_size, std::uint64_t{2} << 20U);
    EXPECT_EQ(defaults.table_options.block_size, std::size_t{4} << 10U);
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
