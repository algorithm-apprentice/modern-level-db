#include "table/filter_block.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/result.h"
#include "table/bloom_filter.h"

namespace modern_leveldb {
namespace {

static_assert(!std::is_copy_constructible_v<FilterBlockBuilder>);
static_assert(!std::is_move_constructible_v<FilterBlockBuilder>);
static_assert(!std::is_copy_constructible_v<FilterBlockReader>);
static_assert(!std::is_copy_assignable_v<FilterBlockReader>);
static_assert(std::is_nothrow_move_constructible_v<FilterBlockReader>);
static_assert(!std::is_move_assignable_v<FilterBlockReader>);

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

// A filter block with the given filter bytes, filter offsets, and array offset.
std::vector<std::byte> RawBlock(std::vector<std::byte> data,
                                std::initializer_list<std::uint32_t> offsets,
                                std::uint32_t array_offset, unsigned int base_lg = 11) {
  for (const std::uint32_t offset : offsets) {
    AppendFixed32(data, offset);
  }
  AppendFixed32(data, array_offset);
  data.push_back(static_cast<std::byte>(base_lg));
  return data;
}

FilterBlockReader MakeReader(ByteView block) {
  auto reader = FilterBlockReader::Create(Materialize(block), BloomFilterPolicy(10));
  EXPECT_TRUE(reader.has_value()) << reader.error().ToString();
  return std::move(reader).value();
}

void ExpectCorruptBlock(std::vector<std::byte> contents) {
  const Result<FilterBlockReader> reader =
      FilterBlockReader::Create(std::move(contents), BloomFilterPolicy(10));
  ASSERT_FALSE(reader.has_value());
  EXPECT_EQ(reader.error().code(), ErrorCode::Corruption);
}

void ExpectInvalidArgument(const Status& status) {
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
}

bool Matches(const FilterBlockReader& reader, std::uint64_t block_offset, std::string_view key) {
  return reader.KeyMayMatch(block_offset, AsBytes(key));
}

TEST(FilterBlockTest, EmptyBuilderWritesLevelDbBytes) {
  FilterBlockBuilder unstarted(BloomFilterPolicy(10));
  EXPECT_EQ(Materialize(unstarted.Finish()), Bytes({0x00, 0x00, 0x00, 0x00, 0x0b}));

  FilterBlockBuilder builder(BloomFilterPolicy(10));
  ASSERT_TRUE(builder.StartBlock(0).has_value());
  const std::vector<std::byte> block = Materialize(builder.Finish());

  EXPECT_EQ(block, Bytes({0x00, 0x00, 0x00, 0x00, 0x0b}));
  const FilterBlockReader reader = MakeReader(block);
  EXPECT_TRUE(Matches(reader, 0, "foo"));
  EXPECT_TRUE(Matches(reader, 100'000, "foo"));
}

TEST(FilterBlockTest, BlocksInOneRangeShareAFilter) {
  FilterBlockBuilder builder(BloomFilterPolicy(10));
  ASSERT_TRUE(builder.StartBlock(100).has_value());
  ASSERT_TRUE(builder.AddKey(AsBytes("foo")).has_value());
  ASSERT_TRUE(builder.AddKey(AsBytes("bar")).has_value());
  ASSERT_TRUE(builder.AddKey(AsBytes("box")).has_value());
  ASSERT_TRUE(builder.StartBlock(200).has_value());
  ASSERT_TRUE(builder.AddKey(AsBytes("box")).has_value());
  ASSERT_TRUE(builder.StartBlock(300).has_value());
  ASSERT_TRUE(builder.AddKey(AsBytes("hello")).has_value());
  const std::vector<std::byte> block = Materialize(builder.Finish());

  EXPECT_EQ(block, Bytes({0xa1, 0x49, 0x12, 0x05, 0x0c, 0x10, 0x62, 0x48, 0x06, 0x00, 0x00, 0x00,
                          0x00, 0x09, 0x00, 0x00, 0x00, 0x0b}));
  const FilterBlockReader reader = MakeReader(block);
  EXPECT_TRUE(Matches(reader, 100, "foo"));
  EXPECT_TRUE(Matches(reader, 100, "bar"));
  EXPECT_TRUE(Matches(reader, 100, "box"));
  EXPECT_TRUE(Matches(reader, 100, "hello"));
  EXPECT_FALSE(Matches(reader, 100, "missing"));
  EXPECT_FALSE(Matches(reader, 100, "other"));
}

TEST(FilterBlockTest, EachRangeHasItsOwnFilter) {
  FilterBlockBuilder builder(BloomFilterPolicy(10));
  ASSERT_TRUE(builder.StartBlock(0).has_value());
  ASSERT_TRUE(builder.AddKey(AsBytes("foo")).has_value());
  ASSERT_TRUE(builder.StartBlock(2000).has_value());
  ASSERT_TRUE(builder.AddKey(AsBytes("bar")).has_value());
  ASSERT_TRUE(builder.StartBlock(3100).has_value());
  ASSERT_TRUE(builder.AddKey(AsBytes("box")).has_value());
  ASSERT_TRUE(builder.StartBlock(9000).has_value());
  ASSERT_TRUE(builder.AddKey(AsBytes("box")).has_value());
  ASSERT_TRUE(builder.AddKey(AsBytes("hello")).has_value());
  const std::vector<std::byte> block = Materialize(builder.Finish());

  // clang-format off
  EXPECT_EQ(block, Bytes({
      0x21, 0x49, 0x12, 0x00, 0x00, 0x10, 0x42, 0x08, 0x06,
      0x80, 0x00, 0x00, 0x04, 0x08, 0x10, 0x20, 0x40, 0x06,
      0x81, 0x40, 0x00, 0x05, 0x0c, 0x10, 0x60, 0x40, 0x06,
      0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
      0x12, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
      0x1b, 0x00, 0x00, 0x00, 0x0b,
  }));
  // clang-format on
  const FilterBlockReader reader = MakeReader(block);

  EXPECT_TRUE(Matches(reader, 0, "foo"));
  EXPECT_TRUE(Matches(reader, 2000, "bar"));
  EXPECT_FALSE(Matches(reader, 0, "box"));
  EXPECT_FALSE(Matches(reader, 0, "hello"));

  EXPECT_TRUE(Matches(reader, 3100, "box"));
  EXPECT_FALSE(Matches(reader, 3100, "foo"));
  EXPECT_FALSE(Matches(reader, 3100, "bar"));
  EXPECT_FALSE(Matches(reader, 3100, "hello"));

  EXPECT_FALSE(Matches(reader, 4100, "foo"));
  EXPECT_FALSE(Matches(reader, 4100, "box"));

  EXPECT_TRUE(Matches(reader, 9000, "box"));
  EXPECT_TRUE(Matches(reader, 9000, "hello"));
  EXPECT_FALSE(Matches(reader, 9000, "foo"));
  EXPECT_FALSE(Matches(reader, 9000, "bar"));

  // An offset beyond the last filter may match.
  EXPECT_TRUE(Matches(reader, 10'240, "foo"));
}

TEST(FilterBlockTest, RejectsBlockOffsetsThatOverflowTheOffsetArray) {
  FilterBlockBuilder builder(BloomFilterPolicy(10));
  ASSERT_TRUE(builder.StartBlock(0).has_value());

  ExpectInvalidArgument(builder.StartBlock(std::numeric_limits<std::uint64_t>::max()));
  ExpectInvalidArgument(builder.StartBlock(std::uint64_t{1} << 42U));

  EXPECT_EQ(Materialize(builder.Finish()), Bytes({0x00, 0x00, 0x00, 0x00, 0x0b}));
}

TEST(FilterBlockTest, RejectsKeysThatOverflowTheFilterBlock) {
  // Seven keys fit below 4 GiB at this density; the eighth would not. No
  // filter is generated, so nothing large is allocated.
  const BloomFilterPolicy dense(std::numeric_limits<std::uint32_t>::max());
  FilterBlockBuilder builder(dense);
  for (int key = 0; key < 7; ++key) {
    ASSERT_TRUE(builder.AddKey(AsBytes("k")).has_value()) << key;
  }

  ExpectInvalidArgument(builder.AddKey(AsBytes("k")));
  ExpectInvalidArgument(builder.AddKey(AsBytes("k")));
}

TEST(FilterBlockTest, RejectsRangesThatOverflowWithPendingKeys) {
  const BloomFilterPolicy dense(std::numeric_limits<std::uint32_t>::max());
  FilterBlockBuilder builder(dense);
  for (int key = 0; key < 7; ++key) {
    ASSERT_TRUE(builder.AddKey(AsBytes("k")).has_value()) << key;
  }

  // Closing the pending filter adds offset entries for 200 million ranges.
  ExpectInvalidArgument(builder.StartBlock(std::uint64_t{200'000'000} * 2048));
}

TEST(FilterBlockReaderTest, RejectsMalformedBlocks) {
  const std::vector<std::byte> filter(9, std::byte{0});

  ExpectCorruptBlock({});
  ExpectCorruptBlock(Bytes({0x00, 0x00, 0x00, 0x0b}));
  ExpectCorruptBlock(RawBlock({}, {}, 0, 10));
  ExpectCorruptBlock(RawBlock({}, {}, 1));
  ExpectCorruptBlock(RawBlock(Bytes({0x00, 0x00, 0x00}), {}, 0));
  ExpectCorruptBlock(RawBlock(Bytes({'a', 'b', 'c'}), {}, 3));
  ExpectCorruptBlock(RawBlock(filter, {1}, 9));
  ExpectCorruptBlock(RawBlock(filter, {0, 5, 3}, 9));
  ExpectCorruptBlock(RawBlock(filter, {0, 10}, 9));
}

TEST(FilterBlockReaderTest, AcceptsLevelDbLayouts) {
  const std::vector<std::byte> filter(9, std::byte{0});

  EXPECT_TRUE(FilterBlockReader::Create(RawBlock({}, {}, 0), BloomFilterPolicy(10)).has_value());
  EXPECT_TRUE(
      FilterBlockReader::Create(RawBlock(filter, {0, 9, 9}, 9), BloomFilterPolicy(10)).has_value());
}

}  // namespace
}  // namespace modern_leveldb
