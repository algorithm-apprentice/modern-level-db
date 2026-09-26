#include "table/compression.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <random>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

std::vector<std::byte> Bytes(std::initializer_list<unsigned int> values) {
  std::vector<std::byte> result;
  result.reserve(values.size());
  for (const unsigned int value : values) {
    result.push_back(static_cast<std::byte>(value));
  }
  return result;
}

template <typename T>
void ExpectCorruption(const Result<T>& result) {
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::Corruption) << result.error().ToString();
}

static_assert(static_cast<std::uint8_t>(BlockCompression::None) == 0);
static_assert(static_cast<std::uint8_t>(BlockCompression::Snappy) == 1);
static_assert(static_cast<std::uint8_t>(BlockCompression::Zstd) == 2);

TEST(BlockCompressionTest, UsesTheStrictLevelDbSavingsThreshold) {
  EXPECT_FALSE(CompressionIsWorthwhile(0, 0));
  EXPECT_FALSE(CompressionIsWorthwhile(8, 7));
  EXPECT_TRUE(CompressionIsWorthwhile(8, 6));
  EXPECT_FALSE(CompressionIsWorthwhile(16, 14));
  EXPECT_TRUE(CompressionIsWorthwhile(16, 13));
  EXPECT_FALSE(CompressionIsWorthwhile(std::numeric_limits<std::size_t>::max(),
                                       std::numeric_limits<std::size_t>::max()));
}

TEST(BlockCompressionTest, CompressesAndDecompressesSnappyAndZstd) {
  const std::vector<std::byte> raw(16 * 1024, std::byte{'x'});
  std::vector<std::byte> scratch;

  for (const BlockCompression type : {BlockCompression::Snappy, BlockCompression::Zstd}) {
    SCOPED_TRACE(static_cast<int>(type));
    ASSERT_TRUE(TryCompressBlock(raw, type, 1, scratch));
    EXPECT_TRUE(CompressionIsWorthwhile(raw.size(), scratch.size()));

    const Result<std::vector<std::byte>> decoded = DecompressBlock(scratch, type);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().ToString();
    EXPECT_EQ(*decoded, raw);
  }
}

TEST(BlockCompressionTest, FallsBackForNoneEmptyAndIncompressibleInput) {
  std::vector<std::byte> scratch(10, std::byte{0xff});
  EXPECT_FALSE(TryCompressBlock({}, BlockCompression::None, 1, scratch));
  EXPECT_TRUE(scratch.empty());
  EXPECT_FALSE(TryCompressBlock({}, BlockCompression::Snappy, 1, scratch));
  EXPECT_TRUE(scratch.empty());
  EXPECT_FALSE(TryCompressBlock({}, BlockCompression::Zstd, 1, scratch));
  EXPECT_TRUE(scratch.empty());

  std::mt19937_64 random(20260925);
  std::vector<std::byte> raw(16 * 1024);
  for (std::byte& value : raw) {
    value = static_cast<std::byte>(random());
  }
  EXPECT_FALSE(TryCompressBlock(raw, BlockCompression::Snappy, 1, scratch));
  EXPECT_TRUE(scratch.empty());
  EXPECT_FALSE(TryCompressBlock(raw, BlockCompression::Zstd, 1, scratch));
  EXPECT_TRUE(scratch.empty());
}

TEST(BlockCompressionTest, DecodesIndependentRawSnappyStreams) {
  const std::vector<std::byte> literal = Bytes({0x05, 0x10, 'h', 'e', 'l', 'l', 'o'});
  const Result<std::vector<std::byte>> decoded = DecompressBlock(literal, BlockCompression::Snappy);
  ASSERT_TRUE(decoded.has_value()) << decoded.error().ToString();
  EXPECT_EQ(*decoded, Bytes({'h', 'e', 'l', 'l', 'o'}));

  const Result<std::vector<std::byte>> empty =
      DecompressBlock(Bytes({0x00}), BlockCompression::Snappy);
  ASSERT_TRUE(empty.has_value()) << empty.error().ToString();
  EXPECT_TRUE(empty->empty());
}

TEST(BlockCompressionTest, DecodesIndependentSizedZstdFrames) {
  const std::vector<std::byte> frame = Bytes({0x28, 0xb5, 0x2f, 0xfd, 0x24, 0x05, 0x29, 0x00, 0x00,
                                              'h', 'e', 'l', 'l', 'o', 0xa3, 0x6d, 0x9f, 0x88});
  const Result<std::vector<std::byte>> decoded = DecompressBlock(frame, BlockCompression::Zstd);
  ASSERT_TRUE(decoded.has_value()) << decoded.error().ToString();
  EXPECT_EQ(*decoded, Bytes({'h', 'e', 'l', 'l', 'o'}));
}

TEST(BlockCompressionTest, RejectsMalformedAndUnboundedStreams) {
  ExpectCorruption(DecompressBlock({}, BlockCompression::Snappy));
  ExpectCorruption(DecompressBlock(Bytes({0x05, 0x10, 'h'}), BlockCompression::Snappy));
  ExpectCorruption(DecompressBlock(Bytes({0xff}), BlockCompression::Zstd));
  const std::vector<std::byte> incomplete =
      Bytes({0x28, 0xb5, 0x2f, 0xfd, 0x24, 0x05, 0x29, 0x00, 0x00, 'h'});
  ExpectCorruption(DecompressBlock(incomplete, BlockCompression::Zstd));

  const std::vector<std::byte> unknown_size =
      Bytes({0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x58, 0x29, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o', 0xa3,
             0x6d, 0x9f, 0x88});
  ExpectCorruption(DecompressBlock(unknown_size, BlockCompression::Zstd));

  const std::vector<std::byte> empty_zstd =
      Bytes({0x28, 0xb5, 0x2f, 0xfd, 0x24, 0x00, 0x01, 0x00, 0x00, 0x99, 0xe9, 0xd8, 0x51});
  ExpectCorruption(DecompressBlock(empty_zstd, BlockCompression::Zstd));

  const std::vector<std::byte> over_uint32 =
      Bytes({0x28, 0xb5, 0x2f, 0xfd, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00});
  ExpectCorruption(DecompressBlock(over_uint32, BlockCompression::Zstd));

  ExpectCorruption(DecompressBlock(Bytes({0x00}), BlockCompression::None));
  ExpectCorruption(DecompressBlock(Bytes({0x00}), static_cast<BlockCompression>(0xff)));
}

TEST(BlockCompressionTest, ReusesScratchAndRejectsUnknownRequests) {
  const std::vector<std::byte> raw(16 * 1024, std::byte{'x'});
  std::vector<std::byte> scratch;
  ASSERT_TRUE(TryCompressBlock(raw, BlockCompression::Snappy, 1, scratch));
  const std::byte* data = scratch.data();
  ASSERT_TRUE(TryCompressBlock(raw, BlockCompression::Snappy, 1, scratch));
  EXPECT_EQ(scratch.data(), data);
  EXPECT_FALSE(TryCompressBlock(raw, BlockCompression::None, 1, scratch));
  EXPECT_TRUE(scratch.empty());
  EXPECT_FALSE(TryCompressBlock(raw, static_cast<BlockCompression>(0xff), 1, scratch));
  EXPECT_TRUE(scratch.empty());
}

}  // namespace
}  // namespace modern_leveldb
