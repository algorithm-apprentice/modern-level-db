#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/database.h"
#include "format/write_batch.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "support/temporary_directory.h"

namespace modern_leveldb {
namespace {

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
    ASSERT_TRUE(batch.Put(AsBytes("a"), AsBytes("1")).has_value());
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
  options.write_buffer_size = 64 * 1'024;
#if defined(_WIN32)
  options.allow_weak_namespace_durability = true;
#endif
  auto opened = DatabaseEngine::Open(options, directory.path() / "db");
  ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
  auto& database = **opened;
  const std::string first(80 * 1'024, 'a');
  EncodedWriteBatch batch;
  ASSERT_TRUE(batch.Put(AsBytes("shared"), AsBytes(first)).has_value());
  ASSERT_TRUE(database.Write(batch, true).has_value());
  ASSERT_TRUE(database.FlushMemTable().has_value());
  ASSERT_TRUE(database.WaitForBackgroundWork().has_value());
  const auto snapshot = database.GetSnapshot();
  auto iterator = database.NewIterator({.snapshot = snapshot});
  ASSERT_TRUE(iterator->SeekToFirst().has_value());
  ASSERT_TRUE(iterator->valid());
  std::string latest;
  for (unsigned generation = 1; generation < 12; ++generation) {
    latest.assign(80 * 1'024, static_cast<char>('a' + generation));
    EncodedWriteBatch next;
    ASSERT_TRUE(next.Put(AsBytes("shared"), AsBytes(latest)).has_value());
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
  ASSERT_TRUE(database.Get(AsBytes("shared"), value, {.snapshot = snapshot}).value());
  EXPECT_EQ(AsStringView(value), first);
  EXPECT_EQ(AsStringView(iterator->value()), first);
  database.ReleaseSnapshot(snapshot);
}

}  // namespace
}  // namespace modern_leveldb
