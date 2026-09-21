#include "modern_leveldb/base/result.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace modern_leveldb {
namespace {

TEST(ResultTest, DefaultStatusIsSuccessful) {
  const Status status;

  EXPECT_TRUE(status.has_value());
}

TEST(ResultTest, StoresSuccessfulValue) {
  const Result<std::string> result = std::string("value");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "value");
}

TEST(ResultTest, StoresTypedError) {
  const Result<std::uint64_t> result = std::unexpected(Error::NotFound("missing database key"));

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kNotFound);
  EXPECT_EQ(result.error().message(), "missing database key");
}

TEST(ResultTest, FormatsErrorForDiagnostics) {
  const Error error = Error::InvalidArgument("block size must be positive");

  EXPECT_EQ(error.ToString(), "invalid_argument: block size must be positive");
}

TEST(ResultTest, CapturesFactoryCallSite) {
  const std::uint_least32_t expected_line = __LINE__ + 1;
  const Error error = Error::Corruption("invalid record");

  EXPECT_EQ(error.location().line(), expected_line);
  EXPECT_NE(std::string_view(error.location().file_name()).find("result_test.cc"),
            std::string_view::npos);
}

TEST(ResultTest, ExposesStableErrorCodeNames) {
  EXPECT_EQ(ErrorCodeName(ErrorCode::kNotFound), "not_found");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kCorruption), "corruption");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kInvalidArgument), "invalid_argument");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kIo), "io");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kNotSupported), "not_supported");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kBusy), "busy");
  EXPECT_EQ(ErrorCodeName(ErrorCode::kAborted), "aborted");
}

}  // namespace
}  // namespace modern_leveldb
