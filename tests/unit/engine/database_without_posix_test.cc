#include <gtest/gtest.h>

#include <memory>

#include "engine/database.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

TEST(DatabaseWithoutPosixTest, NeedsAFileSystemFromTheCaller) {
  DatabaseEngineOptions options;
  options.create_if_missing = true;

  const Result<std::unique_ptr<DatabaseEngine>> database = DatabaseEngine::Open(options, "db");

  ASSERT_FALSE(database.has_value());
  EXPECT_EQ(database.error().code(), ErrorCode::NotSupported);
}

}  // namespace
}  // namespace modern_leveldb
