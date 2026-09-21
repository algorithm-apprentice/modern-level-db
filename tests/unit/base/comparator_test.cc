#include "modern_leveldb/base/comparator.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {
namespace {

std::vector<std::byte> OwnedBytes(std::string_view value) {
  const ByteView bytes = AsBytes(value);
  return {bytes.begin(), bytes.end()};
}

TEST(ComparatorTest, OrdersBytesLexicographically) {
  const Comparator& comparator = BytewiseComparator();

  EXPECT_LT(comparator.Compare(AsBytes("abc"), AsBytes("abd")), 0);
  EXPECT_GT(comparator.Compare(AsBytes("abd"), AsBytes("abc")), 0);
  EXPECT_EQ(comparator.Compare(AsBytes("abc"), AsBytes("abc")), 0);
}

TEST(ComparatorTest, OrdersPrefixesBeforeLongerKeys) {
  const Comparator& comparator = BytewiseComparator();

  EXPECT_LT(comparator.Compare(AsBytes("abc"), AsBytes("abcd")), 0);
  EXPECT_GT(comparator.Compare(AsBytes("abcd"), AsBytes("abc")), 0);
}

TEST(ComparatorTest, ComparesEmbeddedNullBytes) {
  const std::array left{std::byte{'a'}, std::byte{0}, std::byte{'b'}};
  const std::array right{std::byte{'a'}, std::byte{0}, std::byte{'c'}};

  EXPECT_LT(BytewiseComparator().Compare(left, right), 0);
}

TEST(ComparatorTest, UsesLevelDbCompatibleName) {
  EXPECT_EQ(BytewiseComparator().Name(), "leveldb.BytewiseComparator");
}

TEST(ComparatorTest, ShortensSeparatorWhenOrderingIsPreserved) {
  std::vector<std::byte> start = OwnedBytes("abc1suffix");

  BytewiseComparator().FindShortestSeparator(start, AsBytes("abc3"));

  EXPECT_EQ(start, OwnedBytes("abc2"));
}

TEST(ComparatorTest, LeavesAdjacentSeparatorUnchanged) {
  std::vector<std::byte> start = OwnedBytes("abc1suffix");

  BytewiseComparator().FindShortestSeparator(start, AsBytes("abc2"));

  EXPECT_EQ(start, OwnedBytes("abc1suffix"));
}

TEST(ComparatorTest, ShortensSuccessorAtFirstIncrementableByte) {
  std::vector<std::byte> key{
      std::byte{0xff},
      std::byte{'a'},
      std::byte{'z'},
  };

  BytewiseComparator().FindShortSuccessor(key);

  EXPECT_EQ(key, (std::vector<std::byte>{std::byte{0xff}, std::byte{'b'}}));
}

TEST(ComparatorTest, LeavesAllFFSuccessorUnchanged) {
  std::vector<std::byte> key{std::byte{0xff}, std::byte{0xff}};

  BytewiseComparator().FindShortSuccessor(key);

  EXPECT_EQ(key, (std::vector<std::byte>{std::byte{0xff}, std::byte{0xff}}));
}

}  // namespace
}  // namespace modern_leveldb
