#include "modern_leveldb/base/coding.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

TEST(CodingTest, EncodesAndDecodesFixedValuesInPreallocatedStorage) {
  std::array<std::byte, 12> storage;

  EncodeFixed64(std::span<std::byte, 8>(storage.data(), 8), 0x0102030405060708ULL);
  EncodeFixed32(std::span<std::byte, 4>(storage.data() + 8, 4), 0x0a0b0c0dU);

  EXPECT_EQ(storage,
            (std::array{
                std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
                std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
                std::byte{0x0d}, std::byte{0x0c}, std::byte{0x0b}, std::byte{0x0a},
            }));
  EXPECT_EQ(DecodeFixed64(std::span<const std::byte, 8>(storage.data(), 8)),
            0x0102030405060708ULL);
  EXPECT_EQ(DecodeFixed32(std::span<const std::byte, 4>(storage.data() + 8, 4)),
            0x0a0b0c0dU);
}

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

TEST(CodingTest, MatchesFixed64GoldenVector) {
  constexpr std::uint64_t Value = 0xfedcba9876543210ULL;
  const std::vector<std::byte> golden{
      std::byte{0x10}, std::byte{0x32}, std::byte{0x54}, std::byte{0x76},
      std::byte{0x98}, std::byte{0xba}, std::byte{0xdc}, std::byte{0xfe},
  };
  std::vector<std::byte> output;
  AppendFixed64(output, Value);
  EXPECT_EQ(output, golden);

  for (std::size_t size = 0; size < golden.size(); ++size) {
    SCOPED_TRACE(size);
    ByteView input = ByteView(golden).first(size);
    const auto decoded = ConsumeFixed64(input);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
    EXPECT_EQ(input.data(), golden.data());
    EXPECT_EQ(input.size(), size);
  }

  std::vector<std::byte> storage = golden;
  storage.push_back(std::byte{0x55});
  ByteView input = storage;
  const auto decoded = ConsumeFixed64(input);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, Value);
  ASSERT_EQ(input.size(), 1U);
  EXPECT_EQ(input.front(), std::byte{0x55});
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

TEST(CodingTest, DecodesMaximumVarint32GoldenVector) {
  const std::vector<std::byte> golden{
      std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x0f},
  };
  for (std::size_t size = 0; size < golden.size(); ++size) {
    SCOPED_TRACE(size);
    ByteView input = ByteView(golden).first(size);
    const auto decoded = ConsumeVarint32(input);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
    EXPECT_EQ(input.data(), golden.data());
    EXPECT_EQ(input.size(), size);
  }

  std::vector<std::byte> storage = golden;
  storage.push_back(std::byte{0x55});
  ByteView input = storage;
  const auto decoded = ConsumeVarint32(input);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, std::numeric_limits<std::uint32_t>::max());
  ASSERT_EQ(input.size(), 1U);
  EXPECT_EQ(input.front(), std::byte{0x55});
}

TEST(CodingTest, MatchesVarint64GoldenVectors) {
  struct GoldenVector {
    std::uint64_t value;
    std::vector<std::byte> bytes;
  };
  const std::vector<GoldenVector> vectors{
      {0U, {std::byte{0x00}}},
      {127U, {std::byte{0x7f}}},
      {128U, {std::byte{0x80}, std::byte{0x01}}},
      {std::uint64_t{1} << 32U,
       {std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x10}}},
      {std::numeric_limits<std::uint64_t>::max(),
       {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x01}}},
  };

  for (const auto& golden : vectors) {
    SCOPED_TRACE(golden.value);
    std::vector<std::byte> output;
    AppendVarint64(output, golden.value);
    EXPECT_EQ(output, golden.bytes);

    for (std::size_t size = 0; size < golden.bytes.size(); ++size) {
      SCOPED_TRACE(size);
      ByteView input = ByteView(golden.bytes).first(size);
      const auto decoded = ConsumeVarint64(input);
      ASSERT_FALSE(decoded.has_value());
      EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
      EXPECT_EQ(input.data(), golden.bytes.data());
      EXPECT_EQ(input.size(), size);
    }

    std::vector<std::byte> storage = golden.bytes;
    storage.push_back(std::byte{0x55});
    ByteView input = storage;
    const auto decoded = ConsumeVarint64(input);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, golden.value);
    ASSERT_EQ(input.size(), 1U);
    EXPECT_EQ(input.front(), std::byte{0x55});
  }
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

TEST(CodingTest, TruncatesTerminalVarint32PayloadLikeLevelDb) {
  const std::vector input_storage{
      std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x10},
  };
  ByteView input = input_storage;

  const Result<std::uint32_t> value = ConsumeVarint32(input);

  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 0U);
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, TruncatesTerminalVarint64PayloadLikeLevelDb) {
  const std::vector input_storage{
      std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
      std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x02},
  };
  ByteView input = input_storage;

  const Result<std::uint64_t> value = ConsumeVarint64(input);

  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, std::numeric_limits<std::uint64_t>::max() >> 1U);
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, RejectsOverlongVarint32WithoutConsumingInput) {
  std::vector<std::byte> storage(6, std::byte{0x80});
  storage.back() = std::byte{0};
  ByteView input = storage;

  const auto decoded = ConsumeVarint32(input);

  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
  EXPECT_EQ(input.data(), storage.data());
  EXPECT_EQ(input.size(), storage.size());
}

TEST(CodingTest, RejectsOverlongVarint64WithoutConsumingInput) {
  std::vector<std::byte> storage(11, std::byte{0x80});
  storage.back() = std::byte{0};
  ByteView input = storage;

  const auto decoded = ConsumeVarint64(input);

  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
  EXPECT_EQ(input.data(), storage.data());
  EXPECT_EQ(input.size(), storage.size());
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

TEST(CodingTest, AppendsLengthPrefixedBytesFromSameVector) {
  for (const bool reallocate : {false, true}) {
    SCOPED_TRACE(reallocate);
    std::vector<std::byte> output{std::byte{'a'}, std::byte{0}, std::byte{'b'}};
    if (reallocate) {
      output.resize(output.capacity(), std::byte{'x'});
    } else {
      output.reserve(64);
    }
    const std::vector<std::byte> original = output;
    std::vector<std::byte> expected = original;
    AppendVarint32(expected, static_cast<std::uint32_t>(original.size()));
    expected.insert(expected.end(), original.begin(), original.end());

    ASSERT_TRUE(AppendLengthPrefixed(output, ByteView(output)));

    EXPECT_EQ(output, expected);
  }
}

TEST(CodingTest, AppendsLengthPrefixedBytesFromSubspan) {
  for (const bool reallocate : {false, true}) {
    SCOPED_TRACE(reallocate);
    std::vector<std::byte> output{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, std::byte{'d'}};
    if (reallocate) {
      output.resize(output.capacity(), std::byte{'x'});
    } else {
      output.reserve(64);
    }
    std::vector<std::byte> expected = output;
    expected.insert(expected.end(), {std::byte{2}, std::byte{'b'}, std::byte{'c'}});

    ASSERT_TRUE(AppendLengthPrefixed(output, ByteView(output).subspan(1, 2)));

    EXPECT_EQ(output, expected);
  }
}

TEST(CodingTest, AppendsEmptyLengthPrefixedBytes) {
  std::vector<std::byte> output;
  ASSERT_TRUE(AppendLengthPrefixed(output, {}));
  EXPECT_EQ(output, (std::vector<std::byte>{std::byte{0}}));

  output.resize(output.capacity(), std::byte{'x'});
  std::vector<std::byte> expected = output;
  expected.push_back(std::byte{0});

  ASSERT_TRUE(AppendLengthPrefixed(output, ByteView(output).subspan(output.size())));

  EXPECT_EQ(output, expected);
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
