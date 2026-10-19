#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/database.h"
#include "format/internal_key.h"
#include "format/write_batch.h"
#include "metadata/version_set.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "stress/mixed_stress.h"
#include "support/temporary_directory.h"

namespace modern_leveldb {
namespace {

std::size_t PhysicalTableCount(const std::filesystem::path& directory) {
    std::size_t result = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".ldb") {
            ++result;
        }
    }
    return result;
}

std::size_t ManifestTableCount(std::filesystem::path directory) {
    auto file_system = CreateDefaultFileSystem(directory,
#if defined(_WIN32)
                                               true,
#else
                                               false,
#endif
                                               true);
    if (!file_system.has_value()) {
        throw std::runtime_error(file_system.error().ToString());
    }
    const InternalKeyComparator comparator(BytewiseComparator());
    auto versions = VersionSet::Recover(**file_system, directory, comparator);
    if (!versions.has_value()) {
        throw std::runtime_error(versions.error().ToString());
    }
    std::size_t result = 0;
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        result += (*versions)->current()->files(level).size();
    }
    return result;
}

TEST(DatabaseNativeTest, OwnsAFileSystemExecutorAndBlockCacheWhenGivenNone) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("modern-leveldb-database-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    DatabaseEngineOptions options;
    options.create_if_missing = true;
#if defined(_WIN32)
    options.allow_weak_namespace_durability = true;
#endif
    {
        auto database = DatabaseEngine::Open(options, directory);
        ASSERT_TRUE(database.has_value()) << database.error().ToString();
        EncodedWriteBatch batch;
        batch.Put(AsBytes("a"), AsBytes("1"));
        ASSERT_TRUE((*database)->Write(batch, false).has_value());
        ASSERT_TRUE((*database)->FlushMemTable().has_value());
        EXPECT_TRUE((*database)->WaitForBackgroundWork().has_value());
    }
    {
        auto database = DatabaseEngine::Open(options, directory);
        ASSERT_TRUE(database.has_value()) << database.error().ToString();
        std::vector<std::byte> value;
        const auto found = (*database)->Get(AsBytes("a"), value);
        ASSERT_TRUE(found.has_value() && *found);
        EXPECT_EQ(std::string(AsStringView(value)), "1");
    }
    std::filesystem::remove_all(directory);
}

TEST(DatabaseNativeTest, CompactionPreservesSnapshotsAndPinnedIteratorFiles) {
    test_support::TemporaryDirectory directory;
    DatabaseEngineOptions options;
    options.create_if_missing = true;
    options.memtable_size_threshold = 64 * 1'024;
#if defined(_WIN32)
    options.allow_weak_namespace_durability = true;
#endif
    auto opened = DatabaseEngine::Open(options, directory.path() / "db");
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    auto& database = **opened;
    const std::string first(80 * 1'024, 'a');
    EncodedWriteBatch batch;
    batch.Put(AsBytes("shared"), AsBytes(first));
    ASSERT_TRUE(database.Write(batch, true).has_value());
    ASSERT_TRUE(database.FlushMemTable().has_value());
    ASSERT_TRUE(database.WaitForBackgroundWork().has_value());
    auto* snapshot = database.GetSnapshot();
    auto iterator = database.NewIterator({.snapshot = snapshot->sequence()});
    ASSERT_TRUE(iterator->SeekToFirst().has_value());
    ASSERT_TRUE(iterator->valid());
    std::string latest;
    for (unsigned generation = 1; generation < 12; ++generation) {
        latest.assign(80 * 1'024, static_cast<char>('a' + generation));
        EncodedWriteBatch next;
        next.Put(AsBytes("shared"), AsBytes(latest));
        ASSERT_TRUE(database.Write(next, true).has_value());
        ASSERT_TRUE(database.FlushMemTable().has_value());
        ASSERT_TRUE(database.WaitForBackgroundWork().has_value());
    }
    const auto state = database.GetState();
    ASSERT_TRUE(state.has_value());
    std::uint64_t current_files = 0;
    for (const auto& level : state->levels) {
        current_files += level.file_count;
    }
    EXPECT_LT(current_files, 12U);
    std::vector<std::byte> value;
    ASSERT_TRUE(database.Get(AsBytes("shared"), value).value());
    EXPECT_EQ(AsStringView(value), latest);
    ASSERT_TRUE(database.Get(AsBytes("shared"), value, {.snapshot = snapshot->sequence()}).value());
    EXPECT_EQ(AsStringView(value), first);
    EXPECT_EQ(AsStringView(iterator->value()), first);
    database.ReleaseSnapshot(snapshot);
}

TEST(DatabaseNativeTest, RemovesUnpinnedCompactionInputsWithoutAnotherOpen) {
    test_support::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "db";
    DatabaseEngineOptions options;
    options.create_if_missing = true;
    options.memtable_size_threshold = 64 * 1'024;
#if defined(_WIN32)
    options.allow_weak_namespace_durability = true;
#endif
    auto opened = DatabaseEngine::Open(options, database_path);
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    auto& database = **opened;
    for (unsigned generation = 0; generation < 24; ++generation) {
        EncodedWriteBatch batch;
        const std::string value(80 * 1'024, static_cast<char>('a' + generation % 26U));
        batch.Put(AsBytes("shared"), AsBytes(value));
        ASSERT_TRUE(database.Write(batch, true).has_value());
        ASSERT_TRUE(database.FlushMemTable().has_value());
        ASSERT_TRUE(database.WaitForBackgroundWork().has_value());
    }

    const Result<DatabaseEngineState> state = database.GetState();
    ASSERT_TRUE(state.has_value()) << state.error().ToString();
    std::size_t live_tables = 0;
    for (const DatabaseEngineLevelState& level : state->levels) {
        live_tables += level.file_count;
    }

    EXPECT_EQ(PhysicalTableCount(database_path), live_tables);
    EXPECT_EQ(state->protected_output_count, 0U);
    EXPECT_FALSE(state->background_work_scheduled);
    EXPECT_FALSE(state->sticky_error.has_value());
}

TEST(DatabaseNativeTest, MixedStressReclaimsDeferredTablesWhenItReopens) {
    test_support::TemporaryDirectory directory;
    stress::MixedStressArguments arguments;
    arguments.database = directory.path() / "db";
    arguments.output = directory.path() / "output";
    arguments.seed = 301;
    arguments.threads = 4;
    arguments.operations_per_thread = 1'000;
    arguments.keys = 16'384;
    arguments.reopens = 2;
    arguments.compression = Compression::None;

    static_cast<void>(
        stress::RunMixedStress(arguments, stress::StressBuildIdentity{"test-revision", false}));

    EXPECT_GE(PhysicalTableCount(arguments.database), ManifestTableCount(arguments.database));

    DatabaseEngineOptions options;
    options.create_if_missing = true;
#if defined(_WIN32)
    options.allow_weak_namespace_durability = true;
#endif
    auto reopened = DatabaseEngine::Open(options, arguments.database);
    ASSERT_TRUE(reopened.has_value()) << reopened.error().ToString();
    reopened->reset();

    EXPECT_EQ(PhysicalTableCount(arguments.database), ManifestTableCount(arguments.database));
}

}  // namespace
}  // namespace modern_leveldb
