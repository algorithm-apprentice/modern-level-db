#include "table/block_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/crc32c.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::uint64_t MaxNumber = std::numeric_limits<std::uint64_t>::max();

std::vector<std::byte> Bytes(std::initializer_list<unsigned int> values) {
  std::vector<std::byte> result;
  result.reserve(values.size());
  for (const unsigned int value : values) {
    result.push_back(static_cast<std::byte>(value));
  }
  return result;
}

std::vector<std::byte> Materialize(ByteView value) {
  return std::vector<std::byte>(value.begin(), value.end());
}

std::vector<std::byte> EncodedHandle(BlockHandle handle) {
  std::vector<std::byte> encoded;
  AppendBlockHandle(encoded, handle);
  return encoded;
}

// Stores contents with an arbitrary type byte and a matching checksum.
std::vector<std::byte> StoredBlock(ByteView contents, unsigned int type) {
  std::vector<std::byte> stored = Materialize(contents);
  stored.push_back(static_cast<std::byte>(type));
  AppendFixed32(stored, MaskCrc32c(Crc32c(stored)));
  return stored;
}

std::vector<std::byte> StoredBlock(ByteView contents) {
  std::vector<std::byte> stored = Materialize(contents);
  const auto trailer = EncodeBlockTrailer(contents);
  stored.insert(stored.end(), trailer.begin(), trailer.end());
  return stored;
}

void ExpectHandle(const BlockHandle& actual, const BlockHandle& expected) {
  EXPECT_EQ(actual.offset, expected.offset);
  EXPECT_EQ(actual.size, expected.size);
}

template <typename T>
void ExpectError(const Result<T>& result, ErrorCode code) {
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), code) << result.error().ToString();
}

TEST(BlockHandleTest, EncodesOffsetThenSize) {
  EXPECT_EQ(EncodedHandle({.offset = 300, .size = 5}), Bytes({0xac, 0x02, 0x05}));
  EXPECT_EQ(EncodedHandle({.offset = 0, .size = 0}), Bytes({0x00, 0x00}));
  EXPECT_EQ(EncodedHandle({.offset = MaxNumber, .size = MaxNumber}).size(),
            BlockHandleMaxEncodedSize);
}

TEST(BlockHandleTest, RoundTripsVarintBoundaries) {
  constexpr std::array<std::uint64_t, 9> Values = {
      0,
      127,
      128,
      16'383,
      16'384,
      std::numeric_limits<std::uint32_t>::max(),
      std::uint64_t{1} << 32U,
      MaxNumber - 1,
      MaxNumber,
  };
  for (const std::uint64_t offset : Values) {
    for (const std::uint64_t size : Values) {
      const BlockHandle handle{.offset = offset, .size = size};
      const std::vector<std::byte> encoded = EncodedHandle(handle);
      ByteView input = encoded;
      const Result<BlockHandle> decoded = ConsumeBlockHandle(input);
      ASSERT_TRUE(decoded.has_value());
      ExpectHandle(*decoded, handle);
      EXPECT_TRUE(input.empty());
    }
  }
}

TEST(BlockHandleTest, LeavesFollowingBytesForTheCaller) {
  std::vector<std::byte> encoded = EncodedHandle({.offset = 7, .size = 9});
  encoded.push_back(std::byte{0x7f});

  ByteView input = encoded;
  const Result<BlockHandle> decoded = ConsumeBlockHandle(input);

  ASSERT_TRUE(decoded.has_value());
  ExpectHandle(*decoded, {.offset = 7, .size = 9});
  EXPECT_EQ(Materialize(input), Bytes({0x7f}));
}

TEST(BlockHandleTest, RejectsTruncatedHandlesWithoutConsumingInput) {
  const std::vector<std::byte> encoded = EncodedHandle({.offset = 300, .size = 300});
  for (std::size_t length = 0; length < encoded.size(); ++length) {
    SCOPED_TRACE(length);
    ByteView input = ByteView(encoded).first(length);
    ExpectError(ConsumeBlockHandle(input), ErrorCode::Corruption);
    EXPECT_EQ(input.size(), length);
  }
}

TEST(FooterTest, EncodesHandlesPaddingAndMagic) {
  std::vector<std::byte> expected = Bytes({0x01, 0x02, 0x03, 0x04});
  expected.resize(2 * BlockHandleMaxEncodedSize);
  const std::vector<std::byte> magic = Bytes({0x57, 0xfb, 0x80, 0x8b, 0x24, 0x75, 0x47, 0xdb});
  expected.insert(expected.end(), magic.begin(), magic.end());

  const auto encoded = EncodeFooter({
      .metaindex = {.offset = 1, .size = 2},
      .index = {.offset = 3, .size = 4},
  });

  EXPECT_EQ(Materialize(encoded), expected);
}

TEST(FooterTest, RoundTripsMaximumHandles) {
  const Footer footer{
      .metaindex = {.offset = MaxNumber, .size = MaxNumber},
      .index = {.offset = MaxNumber, .size = MaxNumber - 1},
  };

  const auto encoded = EncodeFooter(footer);
  const Result<Footer> decoded = DecodeFooter(encoded);

  ASSERT_TRUE(decoded.has_value());
  ExpectHandle(decoded->metaindex, footer.metaindex);
  ExpectHandle(decoded->index, footer.index);
}

TEST(FooterTest, RejectsBadMagicNumbers) {
  for (std::size_t index = FooterSize - sizeof(TableMagicNumber); index < FooterSize; ++index) {
    SCOPED_TRACE(index);
    auto encoded = EncodeFooter({});
    encoded[index] ^= std::byte{0x01};
    ExpectError(DecodeFooter(encoded), ErrorCode::Corruption);
  }
}

TEST(FooterTest, RejectsMalformedHandles) {
  auto metaindex = EncodeFooter({});
  std::fill_n(metaindex.begin(), 2 * BlockHandleMaxEncodedSize, std::byte{0x80});
  ExpectError(DecodeFooter(metaindex), ErrorCode::Corruption);

  auto index = EncodeFooter({});
  std::fill_n(index.begin() + 2, 2 * BlockHandleMaxEncodedSize - 2, std::byte{0x80});
  ExpectError(DecodeFooter(index), ErrorCode::Corruption);
}

TEST(FooterTest, RejectsNonzeroPadding) {
  auto encoded = EncodeFooter({});
  encoded[2 * BlockHandleMaxEncodedSize - 1] = std::byte{0x01};

  ExpectError(DecodeFooter(encoded), ErrorCode::Corruption);
}

TEST(BlockTrailerTest, StoresTypeNoneAndTheMaskedChecksum) {
  const auto trailer = EncodeBlockTrailer(Bytes({'h', 'e', 'l', 'l', 'o'}));

  // LevelDB's trailer for "hello": type 0, then the masked CRC32C of "hello\0".
  EXPECT_EQ(Materialize(trailer), Bytes({0x00, 0x97, 0xa8, 0x8f, 0x83}));
}

TEST(StoredBlockTest, ReturnsVerifiedContentsInTheSameBuffer) {
  const std::vector<std::byte> contents = Bytes({'b', 'l', 'o', 'c', 'k'});
  std::vector<std::byte> stored = StoredBlock(contents);
  const std::byte* const data = stored.data();

  const Result<std::vector<std::byte>> decoded = DecodeStoredBlock(std::move(stored));

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, contents);
  EXPECT_EQ(decoded->data(), data);
}

TEST(StoredBlockTest, AcceptsEmptyContents) {
  const Result<std::vector<std::byte>> decoded = DecodeStoredBlock(StoredBlock({}));

  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->empty());
}

TEST(StoredBlockTest, RejectsInputShorterThanATrailer) {
  ExpectError(DecodeStoredBlock(Bytes({0x00, 0x00, 0x00, 0x00})), ErrorCode::Corruption);
}

TEST(StoredBlockTest, RejectsChecksumMismatches) {
  const std::vector<std::byte> stored = StoredBlock(Bytes({'a', 'b', 'c'}));
  for (std::size_t index = 0; index < stored.size(); ++index) {
    SCOPED_TRACE(index);
    std::vector<std::byte> corrupted = stored;
    corrupted[index] ^= std::byte{0x01};
    ExpectError(DecodeStoredBlock(std::move(corrupted)), ErrorCode::Corruption);
  }
}

TEST(StoredBlockTest, ReportsCompressedBlocksAsNotSupported) {
  const std::vector<std::byte> contents = Bytes({'z', 'z'});

  ExpectError(DecodeStoredBlock(StoredBlock(contents, 1)), ErrorCode::NotSupported);
  ExpectError(DecodeStoredBlock(StoredBlock(contents, 2)), ErrorCode::NotSupported);
}

TEST(StoredBlockTest, RejectsUnknownTypes) {
  const std::vector<std::byte> contents = Bytes({'q'});

  ExpectError(DecodeStoredBlock(StoredBlock(contents, 3)), ErrorCode::Corruption);
  ExpectError(DecodeStoredBlock(StoredBlock(contents, 0xff)), ErrorCode::Corruption);
}

}  // namespace
}  // namespace modern_leveldb
