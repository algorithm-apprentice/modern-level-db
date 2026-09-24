#include "table/bloom_filter.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"

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

std::vector<std::byte> BuildFilter(const BloomFilterPolicy& policy,
                                   std::initializer_list<std::string_view> keys) {
  std::vector<ByteView> views;
  for (const std::string_view key : keys) {
    views.push_back(AsBytes(key));
  }
  std::vector<std::byte> filter;
  policy.CreateFilter(views, filter);
  return filter;
}

// LevelDB's test key: the fixed32 encoding of an integer.
std::array<std::byte, 4> IntegerKey(std::uint32_t value) {
  std::array<std::byte, 4> key;
  EncodeFixed32(key, value);
  return key;
}

TEST(BloomFilterTest, UsesLevelDbName) {
  EXPECT_EQ(BloomFilterPolicy(10).Name(), "leveldb.BuiltinBloomFilter2");
}

TEST(BloomFilterTest, MatchesLevelDbBytes) {
  const BloomFilterPolicy policy(10);

  EXPECT_EQ(BuildFilter(policy, {"hello", "world"}),
            Bytes({0x11, 0x40, 0x00, 0x41, 0x44, 0x10, 0x40, 0x10, 0x06}));
}

TEST(BloomFilterTest, EmptyFilterMatchesNothing) {
  const BloomFilterPolicy policy(10);
  const std::vector<std::byte> filter = BuildFilter(policy, {});

  EXPECT_EQ(filter, Bytes({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06}));
  EXPECT_FALSE(policy.KeyMayMatch(AsBytes("hello"), filter));
  EXPECT_FALSE(policy.KeyMayMatch(AsBytes("world"), filter));
}

TEST(BloomFilterTest, SmallFilterMatchesItsKeys) {
  const BloomFilterPolicy policy(10);
  const std::vector<std::byte> filter = BuildFilter(policy, {"hello", "world"});

  EXPECT_TRUE(policy.KeyMayMatch(AsBytes("hello"), filter));
  EXPECT_TRUE(policy.KeyMayMatch(AsBytes("world"), filter));
  EXPECT_FALSE(policy.KeyMayMatch(AsBytes("x"), filter));
  EXPECT_FALSE(policy.KeyMayMatch(AsBytes("foo"), filter));
}

TEST(BloomFilterTest, ClampsTheProbeCountLikeLevelDb) {
  constexpr std::array<std::pair<std::uint32_t, unsigned int>, 9> Cases = {{
      {0, 1},
      {1, 1},
      {2, 1},
      {3, 2},
      {10, 6},
      {43, 29},
      {44, 30},
      {100, 30},
      {std::numeric_limits<std::uint32_t>::max(), 30},
  }};
  for (const auto& [bits_per_key, probes] : Cases) {
    SCOPED_TRACE(bits_per_key);
    const std::vector<std::byte> filter = BuildFilter(BloomFilterPolicy(bits_per_key), {});
    EXPECT_EQ(std::to_integer<unsigned int>(filter.back()), probes);
  }
}

TEST(BloomFilterTest, SizesFiltersLikeLevelDb) {
  const BloomFilterPolicy policy(10);

  EXPECT_EQ(policy.FilterSize(0), 9U);
  EXPECT_EQ(policy.FilterSize(6), 9U);
  EXPECT_EQ(policy.FilterSize(7), 10U);
  EXPECT_EQ(policy.FilterSize(100), 126U);
  EXPECT_EQ(BloomFilterPolicy(0).FilterSize(1000), 9U);
  EXPECT_EQ(BuildFilter(policy, {"a", "b", "c", "d", "e", "f", "g"}).size(), policy.FilterSize(7));
}

TEST(BloomFilterTest, SaturatesImpossibleFilterSizes) {
  if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
    GTEST_SKIP() << "a key count cannot overflow 64 bits here";
  }
  EXPECT_EQ(BloomFilterPolicy(10).FilterSize(std::numeric_limits<std::size_t>::max()),
            (std::uint64_t{1} << 61U) + 1);
}

TEST(BloomFilterTest, AppendsWithoutChangingExistingBytes) {
  const BloomFilterPolicy policy(10);
  const std::array keys{AsBytes("hello"), AsBytes("world")};
  std::vector<std::byte> output = Bytes({0xaa, 0xbb});

  policy.CreateFilter(keys, output);

  EXPECT_EQ(output, Bytes({0xaa, 0xbb, 0x11, 0x40, 0x00, 0x41, 0x44, 0x10, 0x40, 0x10, 0x06}));
}

TEST(BloomFilterTest, KeepsTheFalsePositiveRateLow) {
  const BloomFilterPolicy policy(10);
  for (const std::uint32_t length : {1U, 5U, 10U, 50U, 100U, 500U, 1'000U, 5'000U, 10'000U}) {
    SCOPED_TRACE(length);
    std::vector<std::array<std::byte, 4>> keys;
    for (std::uint32_t index = 0; index < length; ++index) {
      keys.push_back(IntegerKey(index));
    }
    std::vector<ByteView> views(keys.begin(), keys.end());
    std::vector<std::byte> filter;
    policy.CreateFilter(views, filter);

    EXPECT_LE(filter.size(), length * 10 / 8 + 40);
    for (const ByteView key : views) {
      ASSERT_TRUE(policy.KeyMayMatch(key, filter));
    }
    int false_positives = 0;
    for (std::uint32_t index = 0; index < 10'000; ++index) {
      false_positives += policy.KeyMayMatch(IntegerKey(index + 1'000'000'000), filter) ? 1 : 0;
    }
    EXPECT_LE(false_positives, 200);
  }
}

TEST(BloomFilterTest, MatchesShortAndReservedFiltersLikeLevelDb) {
  const BloomFilterPolicy policy(10);
  const ByteView key = AsBytes("key");

  EXPECT_FALSE(policy.KeyMayMatch(key, {}));
  EXPECT_FALSE(policy.KeyMayMatch(key, Bytes({0x01})));
  EXPECT_TRUE(policy.KeyMayMatch(key, Bytes({0x00, 0x00})));
  EXPECT_TRUE(policy.KeyMayMatch(key, Bytes({0x00, 0x1f})));
  EXPECT_TRUE(policy.KeyMayMatch(key, Bytes({0x00, 0xff})));
  EXPECT_FALSE(policy.KeyMayMatch(key, Bytes({0x00, 0x1e})));
}

}  // namespace
}  // namespace modern_leveldb
