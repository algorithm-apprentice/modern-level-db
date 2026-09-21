#include <gtest/gtest.h>

#include <expected>
#include <string>

namespace modern_leveldb {
namespace {

TEST(ToolchainTest, SupportsCpp23Expected) {
  const std::expected<int, std::string> value = 42;

  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 42);
}

}  // namespace
}  // namespace modern_leveldb
