#include "modern_leveldb/base/coding.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

TEST(CodingTest, EncodesFixed32InLittleEndianOrder) {
  std::vector<std::byte> output;

  AppendFixed32(output, 0x78563412U);

  EXPECT_EQ(output, (std::vector<std::byte>{
                        std::byte{0x12},
                        std::byte{0x34},
                        std::byte{0x56},
                        std::byte{0x78},
                    }));
}

TEST(CodingTest, DecodesFixed32AndConsumesOnlyItsBytes) {
  const std::vector input_storage{
      std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}, std::byte{0xff},
  };
  ByteView input = input_storage;

  const Result<std::uint32_t> value = ConsumeFixed32(input);

  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 0x78563412U);
  ASSERT_EQ(input.size(), 1U);
  EXPECT_EQ(input.front(), std::byte{0xff});
}

TEST(CodingTest, RejectsTruncatedFixed32WithoutConsumingInput) {
  const std::vector input_storage{
      std::byte{0x12},
      std::byte{0x34},
      std::byte{0x56},
  };
  ByteView input = input_storage;

  const Result<std::uint32_t> value = ConsumeFixed32(input);

  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().code(), ErrorCode::Corruption);
  EXPECT_EQ(input.size(), input_storage.size());
}

TEST(CodingTest, RoundTripsFixed64) {
  constexpr std::uint64_t Expected = 0xfedcba9876543210ULL;
  std::vector<std::byte> output;
  AppendFixed64(output, Expected);
  ByteView input = output;

  const Result<std::uint64_t> value = ConsumeFixed64(input);

  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, Expected);
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, EncodesLevelDbCompatibleVarint32Values) {
  std::vector<std::byte> output;

  AppendVarint32(output, 0U);
  AppendVarint32(output, 127U);
  AppendVarint32(output, 128U);
  AppendVarint32(output, 300U);
  AppendVarint32(output, std::numeric_limits<std::uint32_t>::max());

  EXPECT_EQ(output, (std::vector<std::byte>{
                        std::byte{0x00},
                        std::byte{0x7f},
                        std::byte{0x80},
                        std::byte{0x01},
                        std::byte{0xac},
                        std::byte{0x02},
                        std::byte{0xff},
                        std::byte{0xff},
                        std::byte{0xff},
                        std::byte{0xff},
                        std::byte{0x0f},
                    }));
}

TEST(CodingTest, RoundTripsVarint64Boundaries) {
  const std::vector<std::uint64_t> expected{
      0U,
      1U,
      127U,
      128U,
      16384U,
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint64_t>::max(),
  };
  std::vector<std::byte> output;
  for (const std::uint64_t value : expected) {
    AppendVarint64(output, value);
  }
  ByteView input = output;

  for (const std::uint64_t value : expected) {
    const Result<std::uint64_t> decoded = ConsumeVarint64(input);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, value);
  }
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, RejectsTruncatedVarintWithoutConsumingInput) {
  const std::vector input_storage{std::byte{0x80}};
  ByteView input = input_storage;

  const Result<std::uint32_t> value = ConsumeVarint32(input);

  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().code(), ErrorCode::Corruption);
  EXPECT_EQ(input.size(), input_storage.size());
}

TEST(CodingTest, RejectsOverflowingVarint32) {
  const std::vector input_storage{
      std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x10},
  };
  ByteView input = input_storage;

  const Result<std::uint32_t> value = ConsumeVarint32(input);

  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().code(), ErrorCode::Corruption);
  EXPECT_EQ(input.size(), input_storage.size());
}

TEST(CodingTest, RejectsOverflowingVarint64) {
  const std::vector input_storage{
      std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
      std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x02},
  };
  ByteView input = input_storage;

  const Result<std::uint64_t> value = ConsumeVarint64(input);

  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().code(), ErrorCode::Corruption);
  EXPECT_EQ(input.size(), input_storage.size());
}

TEST(CodingTest, RoundTripsLengthPrefixedBytes) {
  std::vector<std::byte> output;
  ASSERT_TRUE(AppendLengthPrefixed(output, AsBytes(std::string_view{"a\0b", 3})));
  output.push_back(std::byte{0xff});
  ByteView input = output;

  const Result<ByteView> value = ConsumeLengthPrefixed(input);

  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(AsStringView(*value), std::string_view("a\0b", 3));
  ASSERT_EQ(input.size(), 1U);
  EXPECT_EQ(input.front(), std::byte{0xff});
}

TEST(CodingTest, RejectsTruncatedLengthPrefixedBytesWithoutConsumingInput) {
  const std::vector input_storage{
      std::byte{0x03},
      std::byte{'a'},
      std::byte{'b'},
  };
  ByteView input = input_storage;

  const Result<ByteView> value = ConsumeLengthPrefixed(input);

  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().code(), ErrorCode::Corruption);
  EXPECT_EQ(input.size(), input_storage.size());
}

TEST(CodingTest, ComputesVarintLength) {
  EXPECT_EQ(VarintLength(0U), 1U);
  EXPECT_EQ(VarintLength(127U), 1U);
  EXPECT_EQ(VarintLength(128U), 2U);
  EXPECT_EQ(VarintLength(16384U), 3U);
  EXPECT_EQ(VarintLength(std::numeric_limits<std::uint64_t>::max()), 10U);
}

}  // namespace
}  // namespace modern_leveldb
