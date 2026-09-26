#include <gtest/gtest.h>

#include "modern_leveldb/base/result.h"
#include "modern_leveldb/db.h"

namespace modern_leveldb {
namespace {

TEST(PublicDatabaseWithoutPosixTest, ReportsTheMissingDefaultFileSystem) {
  Options options;
  options.create_if_missing = true;

  const Result<Database> database = Database::Open(options, "database");

  ASSERT_FALSE(database.has_value());
  EXPECT_EQ(database.error().code(), ErrorCode::NotSupported);
}

}  // namespace
}  // namespace modern_leveldb
