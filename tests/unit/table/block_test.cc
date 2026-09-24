#include "table/block.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "table/block_builder.h"

namespace modern_leveldb {
namespace {

class ReverseComparator final : public Comparator {
 public:
  int Compare(ByteView left, ByteView right) const noexcept override {
    return BytewiseComparator().Compare(right, left);
  }
  std::string_view Name() const noexcept override { return "test.ReverseComparator"; }
  void FindShortestSeparator(std::vector<std::byte>&, ByteView) const override {}
  void FindShortSuccessor(std::vector<std::byte>&) const override {}
};

template <typename T>
concept CreatableWith = requires(std::vector<std::byte> contents, T&& comparator) {
  Block::Create(std::move(contents), std::forward<T>(comparator));
};

static_assert(!std::is_copy_constructible_v<Block>);
static_assert(!std::is_copy_assignable_v<Block>);
static_assert(std::is_nothrow_move_constructible_v<Block>);
static_assert(!std::is_move_assignable_v<Block>);
static_assert(!std::is_move_constructible_v<BlockBuilder>);
static_assert(!std::is_move_assignable_v<BlockBuilder>);
static_assert(CreatableWith<const ReverseComparator&>);
static_assert(!CreatableWith<ReverseComparator>);
static_assert(!std::is_constructible_v<Block::Iterator, Block&&>);
static_assert(!std::is_constructible_v<Block::Iterator, const Block&&>);

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

std::vector<std::byte> EntryBytes(std::uint32_t shared, std::string_view delta,
                                  std::string_view value) {
  std::vector<std::byte> entry;
  AppendVarint32(entry, shared);
  AppendVarint32(entry, static_cast<std::uint32_t>(delta.size()));
  AppendVarint32(entry, static_cast<std::uint32_t>(value.size()));
  const ByteView delta_bytes = AsBytes(delta);
  const ByteView value_bytes = AsBytes(value);
  entry.insert(entry.end(), delta_bytes.begin(), delta_bytes.end());
  entry.insert(entry.end(), value_bytes.begin(), value_bytes.end());
  return entry;
}

std::vector<std::byte> Concat(std::initializer_list<std::vector<std::byte>> parts) {
  std::vector<std::byte> result;
  for (const std::vector<std::byte>& part : parts) {
    result.insert(result.end(), part.begin(), part.end());
  }
  return result;
}

std::vector<std::byte> WithRestarts(std::vector<std::byte> entries,
                                    std::initializer_list<std::uint32_t> restarts) {
  for (const std::uint32_t restart : restarts) {
    AppendFixed32(entries, restart);
  }
  AppendFixed32(entries, static_cast<std::uint32_t>(restarts.size()));
  return entries;
}

// Entries of the golden block: restart points at offsets 0 and 18.
std::vector<std::byte> GoldenEntries() {
  return Concat({
      EntryBytes(0, "apple", "1"),
      EntryBytes(5, "sauce", "2"),
      EntryBytes(0, "banana", "3"),
  });
}

std::vector<std::byte> BuildGoldenBlock() {
  BlockBuilder builder(2);
  EXPECT_TRUE(builder.Add(AsBytes("apple"), AsBytes("1")).has_value());
  EXPECT_TRUE(builder.Add(AsBytes("applesauce"), AsBytes("2")).has_value());
  EXPECT_TRUE(builder.Add(AsBytes("banana"), AsBytes("3")).has_value());
  return Materialize(builder.Finish());
}

Block MakeBlock(std::vector<std::byte> contents,
                const Comparator& comparator = BytewiseComparator()) {
  auto block = Block::Create(std::move(contents), comparator);
  EXPECT_TRUE(block.has_value()) << block.error().ToString();
  return std::move(block).value();
}

void ExpectCorruptBlock(std::vector<std::byte> contents,
                        const Comparator& comparator = BytewiseComparator()) {
  const Result<Block> block = Block::Create(std::move(contents), comparator);
  ASSERT_FALSE(block.has_value());
  EXPECT_EQ(block.error().code(), ErrorCode::Corruption);
}

void ExpectAt(const Block::Iterator& iterator, std::string_view key, std::string_view value) {
  ASSERT_TRUE(iterator.valid());
  EXPECT_EQ(AsStringView(iterator.key()), key);
  EXPECT_EQ(AsStringView(iterator.value()), value);
}

TEST(BlockBuilderTest, EncodesPrefixCompressedEntriesAndRestarts) {
  EXPECT_EQ(BuildGoldenBlock(), WithRestarts(GoldenEntries(), {0, 18}));
}

TEST(BlockBuilderTest, EmptyBuilderFinishesWithOneRestartPoint) {
  BlockBuilder builder(16);

  EXPECT_TRUE(builder.empty());
  EXPECT_EQ(builder.CurrentSizeEstimate(), 8U);
  EXPECT_EQ(Materialize(builder.Finish()), WithRestarts({}, {0}));
}

TEST(BlockBuilderTest, RestartIntervalOneStoresEveryKeyInFull) {
  BlockBuilder builder(1);
  ASSERT_TRUE(builder.Add(AsBytes("key1"), AsBytes("a")).has_value());
  ASSERT_TRUE(builder.Add(AsBytes("key2"), AsBytes("b")).has_value());

  EXPECT_EQ(Materialize(builder.Finish()),
            WithRestarts(Concat({EntryBytes(0, "key1", "a"), EntryBytes(0, "key2", "b")}), {0, 8}));
}

TEST(BlockBuilderTest, EstimatesTheFinishedSize) {
  BlockBuilder builder(2);
  ASSERT_TRUE(builder.Add(AsBytes("apple"), AsBytes("1")).has_value());
  ASSERT_TRUE(builder.Add(AsBytes("applesauce"), AsBytes("2")).has_value());
  ASSERT_TRUE(builder.Add(AsBytes("banana"), AsBytes("3")).has_value());

  const std::size_t estimate = builder.CurrentSizeEstimate();

  EXPECT_FALSE(builder.empty());
  EXPECT_EQ(builder.Finish().size(), estimate);
}

TEST(BlockBuilderTest, ResetStartsANewBlock) {
  BlockBuilder builder(2);
  ASSERT_TRUE(builder.Add(AsBytes("zebra"), AsBytes("z")).has_value());
  (void)builder.Finish();

  builder.Reset();

  EXPECT_TRUE(builder.empty());
  EXPECT_EQ(builder.CurrentSizeEstimate(), 8U);
  ASSERT_TRUE(builder.Add(AsBytes("apple"), AsBytes("1")).has_value());
  ASSERT_TRUE(builder.Add(AsBytes("applesauce"), AsBytes("2")).has_value());
  ASSERT_TRUE(builder.Add(AsBytes("banana"), AsBytes("3")).has_value());
  EXPECT_EQ(Materialize(builder.Finish()), WithRestarts(GoldenEntries(), {0, 18}));
}

TEST(BlockTest, AcceptsBuilderBlocks) {
  BlockBuilder builder(16);

  EXPECT_TRUE(Block::Create(Materialize(builder.Finish()), BytewiseComparator()).has_value());
  EXPECT_TRUE(Block::Create(BuildGoldenBlock(), BytewiseComparator()).has_value());
}

TEST(BlockTest, ReportsWhetherItHasEntries) {
  BlockBuilder builder(16);

  EXPECT_TRUE(MakeBlock(Materialize(builder.Finish())).empty());
  EXPECT_FALSE(MakeBlock(BuildGoldenBlock()).empty());
}

TEST(BlockTest, RejectsBlocksWithoutAValidRestartCount) {
  ExpectCorruptBlock({});
  ExpectCorruptBlock(Bytes({0x00, 0x00, 0x00}));
  ExpectCorruptBlock(WithRestarts({}, {}));
  ExpectCorruptBlock(Bytes({0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00}));
}

TEST(BlockTest, RejectsInvalidRestartPoints) {
  ExpectCorruptBlock(WithRestarts({}, {4}));
  ExpectCorruptBlock(WithRestarts({}, {0, 0}));
  ExpectCorruptBlock(WithRestarts(GoldenEntries(), {9, 18}));
  ExpectCorruptBlock(WithRestarts(GoldenEntries(), {0, 19}));
  ExpectCorruptBlock(WithRestarts(GoldenEntries(), {0, 0}));
  ExpectCorruptBlock(WithRestarts(GoldenEntries(), {0, 18, 9}));
  ExpectCorruptBlock(WithRestarts(GoldenEntries(), {0, 18, 28}));
  ExpectCorruptBlock(WithRestarts(GoldenEntries(), {0, 9}));
}

TEST(BlockTest, RejectsMalformedEntries) {
  ExpectCorruptBlock(WithRestarts(Bytes({0x80}), {0}));
  ExpectCorruptBlock(WithRestarts(Bytes({0x00}), {0}));
  ExpectCorruptBlock(WithRestarts(Bytes({0x00, 0x05}), {0}));
  ExpectCorruptBlock(WithRestarts(Bytes({0x00, 0x05, 0x01, 'a', 'p'}), {0}));
  ExpectCorruptBlock(WithRestarts(Bytes({0x00, 0x01, 0x80, 'a'}), {0}));
  ExpectCorruptBlock(WithRestarts(Bytes({0xff, 0xff, 0xff, 0xff, 0x7f, 0x00, 0x00}), {0}));
  ExpectCorruptBlock(WithRestarts(Concat({EntryBytes(0, "a", ""), EntryBytes(2, "", "")}), {0}));
}

TEST(BlockTest, RejectsKeysThatDoNotStrictlyIncrease) {
  const std::vector<std::byte> ascending =
      WithRestarts(Concat({EntryBytes(0, "a", ""), EntryBytes(0, "b", "")}), {0});

  ExpectCorruptBlock(WithRestarts(Concat({EntryBytes(0, "b", ""), EntryBytes(0, "a", "")}), {0}));
  ExpectCorruptBlock(WithRestarts(Concat({EntryBytes(0, "a", ""), EntryBytes(1, "", "")}), {0}));
  ExpectCorruptBlock(ascending, ReverseComparator());
  EXPECT_TRUE(Block::Create(ascending, BytewiseComparator()).has_value());
}

TEST(BlockIteratorTest, StartsUnpositioned) {
  const Block block = MakeBlock(BuildGoldenBlock());

  const Block::Iterator iterator(block);

  EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, IteratesForwardAndBackward) {
  const Block block = MakeBlock(BuildGoldenBlock());
  Block::Iterator iterator(block);

  iterator.SeekToFirst();
  ExpectAt(iterator, "apple", "1");
  iterator.Next();
  ExpectAt(iterator, "applesauce", "2");
  iterator.Next();
  ExpectAt(iterator, "banana", "3");
  iterator.Next();
  EXPECT_FALSE(iterator.valid());

  iterator.SeekToLast();
  ExpectAt(iterator, "banana", "3");
  iterator.Prev();
  ExpectAt(iterator, "applesauce", "2");
  iterator.Prev();
  ExpectAt(iterator, "apple", "1");
  iterator.Prev();
  EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, SeeksToTheFirstKeyNotLessThanTheTarget) {
  const Block block = MakeBlock(BuildGoldenBlock());
  Block::Iterator iterator(block);

  iterator.Seek(AsBytes(""));
  ExpectAt(iterator, "apple", "1");
  iterator.Seek(AsBytes("apple"));
  ExpectAt(iterator, "apple", "1");
  iterator.Seek(AsBytes("applea"));
  ExpectAt(iterator, "applesauce", "2");
  iterator.Seek(AsBytes("b"));
  ExpectAt(iterator, "banana", "3");
  iterator.Seek(AsBytes("banana"));
  ExpectAt(iterator, "banana", "3");
  iterator.Seek(AsBytes("bananas"));
  EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, ChangesDirectionAcrossRestartPoints) {
  const Block block = MakeBlock(BuildGoldenBlock());
  Block::Iterator iterator(block);

  iterator.Seek(AsBytes("banana"));
  iterator.Prev();
  ExpectAt(iterator, "applesauce", "2");
  iterator.Next();
  ExpectAt(iterator, "banana", "3");
  iterator.Prev();
  iterator.Prev();
  ExpectAt(iterator, "apple", "1");
  iterator.Next();
  ExpectAt(iterator, "applesauce", "2");
}

TEST(BlockIteratorTest, RemainsValidWhenTheBlockMoves) {
  Block block = MakeBlock(BuildGoldenBlock());
  Block::Iterator iterator(block);
  iterator.SeekToFirst();

  const Block moved(std::move(block));

  ExpectAt(iterator, "apple", "1");
  iterator.Next();
  ExpectAt(iterator, "applesauce", "2");
  iterator.SeekToLast();
  ExpectAt(iterator, "banana", "3");
}

TEST(BlockIteratorTest, EmptyBlockHasNoPositions) {
  BlockBuilder builder(16);
  const Block block = MakeBlock(Materialize(builder.Finish()));
  Block::Iterator iterator(block);

  iterator.SeekToFirst();
  EXPECT_FALSE(iterator.valid());
  iterator.SeekToLast();
  EXPECT_FALSE(iterator.valid());
  iterator.Seek(AsBytes("a"));
  EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, UsesTheBlockComparator) {
  const ReverseComparator reverse;
  BlockBuilder builder(1);
  ASSERT_TRUE(builder.Add(AsBytes("c"), AsBytes("3")).has_value());
  ASSERT_TRUE(builder.Add(AsBytes("b"), AsBytes("2")).has_value());
  ASSERT_TRUE(builder.Add(AsBytes("a"), AsBytes("1")).has_value());
  const Block block = MakeBlock(Materialize(builder.Finish()), reverse);
  Block::Iterator iterator(block);

  iterator.Seek(AsBytes("bb"));
  ExpectAt(iterator, "b", "2");
  iterator.Seek(AsBytes("d"));
  ExpectAt(iterator, "c", "3");
  iterator.Seek(AsBytes(""));
  EXPECT_FALSE(iterator.valid());
}

struct ModelEntry {
  std::vector<std::byte> key;
  std::vector<std::byte> value;
};

class BlockModelTest : public testing::Test {
 protected:
  std::vector<std::byte> RandomBytes(std::size_t length, bool small_alphabet) {
    constexpr std::array<unsigned int, 5> Alphabet = {'a', 'b', 'c', 0x00, 0xff};
    std::vector<std::byte> bytes(length);
    for (std::byte& byte : bytes) {
      byte = static_cast<std::byte>(small_alphabet ? Alphabet[Below(Alphabet.size())] : Below(256));
    }
    return bytes;
  }

  std::vector<std::byte> RandomKey() {
    if (Below(8) == 0) {
      std::vector<std::byte> key(200, std::byte{'p'});
      const std::vector<std::byte> suffix = RandomBytes(Below(4), true);
      key.insert(key.end(), suffix.begin(), suffix.end());
      return key;
    }
    return RandomBytes(Below(9), true);
  }

  std::vector<std::byte> RandomValue() {
    return RandomBytes(Below(8) == 0 ? 128 + Below(200) : Below(17), false);
  }

  std::vector<ModelEntry> RandomEntries(const Comparator& comparator) {
    std::vector<std::vector<std::byte>> keys(Below(200));
    for (std::vector<std::byte>& key : keys) {
      key = RandomKey();
    }
    std::ranges::sort(
        keys, [&](ByteView left, ByteView right) { return comparator.Compare(left, right) < 0; });
    const auto duplicates = std::ranges::unique(
        keys, [&](ByteView left, ByteView right) { return comparator.Compare(left, right) == 0; });
    keys.erase(duplicates.begin(), duplicates.end());

    std::vector<ModelEntry> entries;
    entries.reserve(keys.size());
    for (std::vector<std::byte>& key : keys) {
      entries.push_back({.key = std::move(key), .value = RandomValue()});
    }
    return entries;
  }

  std::size_t Below(std::size_t bound) {
    return std::uniform_int_distribution<std::size_t>(0, bound - 1)(random_);
  }

  static void ExpectAtEntry(const Block::Iterator& iterator, const std::vector<ModelEntry>& entries,
                            std::size_t index) {
    if (index == entries.size()) {
      EXPECT_FALSE(iterator.valid());
      return;
    }
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(Materialize(iterator.key()), entries[index].key);
    EXPECT_EQ(Materialize(iterator.value()), entries[index].value);
  }

  void CheckBlock(std::uint32_t restart_interval, const Comparator& comparator) {
    const std::vector<ModelEntry> entries = RandomEntries(comparator);
    BlockBuilder builder(restart_interval);
    for (const ModelEntry& entry : entries) {
      ASSERT_TRUE(builder.Add(entry.key, entry.value).has_value());
    }
    const std::size_t estimate = builder.CurrentSizeEstimate();
    const std::vector<std::byte> contents = Materialize(builder.Finish());
    ASSERT_EQ(contents.size(), estimate);
    const Block block = MakeBlock(contents, comparator);
    Block::Iterator iterator(block);

    iterator.SeekToFirst();
    for (std::size_t index = 0; index <= entries.size(); ++index) {
      ExpectAtEntry(iterator, entries, index);
      if (iterator.valid()) {
        iterator.Next();
      }
    }

    iterator.SeekToLast();
    for (std::size_t remaining = entries.size(); remaining > 0; --remaining) {
      ExpectAtEntry(iterator, entries, remaining - 1);
      iterator.Prev();
    }
    EXPECT_FALSE(iterator.valid());

    for (int seek = 0; seek < 20; ++seek) {
      const std::vector<std::byte> target =
          !entries.empty() && Below(2) == 0 ? entries[Below(entries.size())].key : RandomKey();
      const auto lower = std::ranges::lower_bound(
          entries, ByteView(target),
          [&](ByteView left, ByteView right) { return comparator.Compare(left, right) < 0; },
          [](const ModelEntry& entry) { return ByteView(entry.key); });
      std::size_t index = static_cast<std::size_t>(lower - entries.begin());
      iterator.Seek(target);
      ExpectAtEntry(iterator, entries, index);

      for (int step = 0; step < 8 && index < entries.size(); ++step) {
        if (Below(2) == 0) {
          iterator.Next();
          ++index;
        } else if (index == 0) {
          iterator.Prev();
          index = entries.size();
        } else {
          iterator.Prev();
          --index;
        }
        ExpectAtEntry(iterator, entries, index);
      }
    }
  }

  std::mt19937_64 random_{20260923};
};

TEST_F(BlockModelTest, MatchesAnOrderedModel) {
  const ReverseComparator reverse;
  for (int round = 0; round < 3; ++round) {
    for (const std::uint32_t restart_interval : {1U, 2U, 3U, 16U}) {
      SCOPED_TRACE(testing::Message() << "round " << round << " interval " << restart_interval);
      CheckBlock(restart_interval, BytewiseComparator());
      CheckBlock(restart_interval, reverse);
    }
  }
}

}  // namespace
}  // namespace modern_leveldb
