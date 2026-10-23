#include "modern_leveldb/base/comparator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <random>
#include <string_view>
#include <type_traits>
#include <vector>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {
namespace {

using CompareSignature = int (Comparator::*)(const ByteView&, const ByteView&) const noexcept;
static_assert(std::is_same_v<decltype(&Comparator::Compare), CompareSignature>);

std::vector<std::byte> OwnedBytes(std::string_view value) {
    const ByteView bytes = AsBytes(value);
    return {bytes.begin(), bytes.end()};
}

int ScalarCompare(ByteView left, ByteView right) {
    const std::size_t length = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < length; ++index) {
        const auto a = std::to_integer<unsigned>(left[index]);
        const auto b = std::to_integer<unsigned>(right[index]);
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }
    if (left.size() == right.size()) {
        return 0;
    }
    return left.size() < right.size() ? -1 : 1;
}

TEST(ComparatorTest, HandlesEmptyViewsWithoutRequiringNonNullStorage) {
    const ByteView empty;
    const std::array byte{std::byte{0xff}};
    const ByteView backed_empty = ByteView(byte).first(0);
    EXPECT_EQ(BytewiseComparator().Compare(empty, empty), 0);
    EXPECT_EQ(BytewiseComparator().Compare(empty, backed_empty), 0);
    EXPECT_EQ(BytewiseComparator().Compare(empty, byte), -1);
    EXPECT_EQ(BytewiseComparator().Compare(byte, empty), 1);
}

TEST(ComparatorTest, PreservesNormalizedUnsignedOrderingForEveryBytePair) {
    for (unsigned a = 0; a < 256; ++a) {
        for (unsigned b = 0; b < 256; ++b) {
            const std::array left{static_cast<std::byte>(a)};
            const std::array right{static_cast<std::byte>(b)};
            EXPECT_EQ(BytewiseComparator().Compare(left, right), ScalarCompare(left, right));
        }
    }
}

TEST(ComparatorTest, MatchesScalarOrderingForUnalignedOverlappingAndVariedViews) {
    std::mt19937_64 random(301);
    std::array<std::byte, 258> left{};
    std::array<std::byte, 258> right{};
    for (unsigned trial = 0; trial < 1000; ++trial) {
        for (auto& byte : left) {
            byte = static_cast<std::byte>(random() & 255U);
        }
        right = left;
        const std::size_t mutation = static_cast<std::size_t>(random() % right.size());
        right[mutation] ^= std::byte{0xff};
        const std::size_t left_offset = static_cast<std::size_t>(random() % 8);
        const std::size_t right_offset = static_cast<std::size_t>(random() % 8);
        const std::size_t left_size = static_cast<std::size_t>(random() % 250);
        const std::size_t right_size = static_cast<std::size_t>(random() % 250);
        const ByteView a = ByteView(left).subspan(left_offset, left_size);
        const ByteView b = ByteView(right).subspan(right_offset, right_size);
        const ByteView overlap = ByteView(left).subspan(right_offset, right_size);
        EXPECT_EQ(BytewiseComparator().Compare(a, b), ScalarCompare(a, b));
        EXPECT_EQ(BytewiseComparator().Compare(a, overlap), ScalarCompare(a, overlap));
        EXPECT_EQ(BytewiseComparator().Compare(a, a), 0);
        const ByteView prefix = a.first(a.size() / 2);
        EXPECT_EQ(BytewiseComparator().Compare(prefix, a), ScalarCompare(prefix, a));
        EXPECT_EQ(BytewiseComparator().Compare(a, prefix), ScalarCompare(a, prefix));
    }
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

TEST(ComparatorTest, SeparatorBoundariesAndBorrowedLimitsKeepExactBytes) {
    struct SeparatorCase {
        std::vector<std::byte> start;
        std::vector<std::byte> limit;
        std::vector<std::byte> expected;
    };
    const std::array cases{
        SeparatorCase{{}, {std::byte{0xff}}, {}},
        SeparatorCase{{std::byte{0xff}, std::byte{0x00}},
                      {std::byte{0xfe}},
                      {std::byte{0xff}, std::byte{0x00}}},
        SeparatorCase{{std::byte{0x00}}, {}, {std::byte{0x00}}},
        SeparatorCase{OwnedBytes("ab"), OwnedBytes("abc"), OwnedBytes("ab")},
        SeparatorCase{OwnedBytes("abc"), OwnedBytes("ab"), OwnedBytes("abc")},
        SeparatorCase{OwnedBytes("abc"), OwnedBytes("abc"), OwnedBytes("abc")},
        SeparatorCase{{std::byte{0xfe}, std::byte{0x00}},
                      {std::byte{0xff}},
                      {std::byte{0xfe}, std::byte{0x00}}},
        SeparatorCase{{std::byte{0xfd}, std::byte{0x00}}, {std::byte{0xff}}, {std::byte{0xfe}}},
        SeparatorCase{{std::byte{0x00}, std::byte{0xff}}, {std::byte{0x03}}, {std::byte{0x01}}},
    };
    for (const auto& test : cases) {
        auto start = test.start;
        BytewiseComparator().FindShortestSeparator(start, test.limit);
        EXPECT_EQ(start, test.expected);
    }

    std::vector<std::byte> start{std::byte{0x01}, std::byte{0x03}, std::byte{0xff}};
    const ByteView borrowed_limit = ByteView(start).subspan(1);
    BytewiseComparator().FindShortestSeparator(start, borrowed_limit);
    EXPECT_EQ(start, (std::vector<std::byte>{std::byte{0x02}}));
}

TEST(ComparatorTest, SuccessorBoundariesKeepExactFirstIncrementablePrefix) {
    struct SuccessorCase {
        std::vector<std::byte> original;
        std::vector<std::byte> expected;
    };
    const std::array cases{
        SuccessorCase{{}, {}},
        SuccessorCase{{std::byte{0xff}, std::byte{0xff}}, {std::byte{0xff}, std::byte{0xff}}},
        SuccessorCase{{std::byte{0xfe}, std::byte{0xff}}, {std::byte{0xff}}},
        SuccessorCase{{std::byte{0x00}, std::byte{0xff}}, {std::byte{0x01}}},
        SuccessorCase{{std::byte{0xff}, std::byte{0xfe}, std::byte{0x00}},
                      {std::byte{0xff}, std::byte{0xff}}},
        SuccessorCase{{std::byte{0xff}, std::byte{0x00}, std::byte{0xff}},
                      {std::byte{0xff}, std::byte{0x01}}},
        SuccessorCase{{std::byte{0x7f}, std::byte{0x00}, std::byte{0xff}}, {std::byte{0x80}}},
        SuccessorCase{{std::byte{0x80}, std::byte{0xfe}}, {std::byte{0x81}}},
    };
    for (const auto& test : cases) {
        auto key = test.original;
        BytewiseComparator().FindShortSuccessor(key);
        EXPECT_EQ(key, test.expected);
    }
}

}  // namespace
}  // namespace modern_leveldb
