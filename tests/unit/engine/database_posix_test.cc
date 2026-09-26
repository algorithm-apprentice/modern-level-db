#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "engine/database.h"
#include "format/write_batch.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

TEST(DatabasePosixTest, OwnsAFileSystemExecutorAndBlockCacheWhenGivenNone) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("modern-leveldb-database-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  DatabaseEngineOptions options;
  options.create_if_missing = true;
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
    const auto value = (*database)->Get(AsBytes("a"));
    ASSERT_TRUE(value.has_value() && value->has_value());
    EXPECT_EQ(std::string(AsStringView(**value)), "1");
  }
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace modern_leveldb
