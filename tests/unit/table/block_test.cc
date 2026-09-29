#include "table/block.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "format/internal_key.h"
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

template <typename... Args>
concept BlockCreatableWith =
    requires(Args&&... args) { Block::Create(std::forward<Args>(args)...); };

static_assert(!std::is_copy_constructible_v<Block>);
static_assert(!std::is_copy_assignable_v<Block>);
static_assert(std::is_nothrow_move_constructible_v<Block>);
static_assert(!std::is_move_assignable_v<Block>);
static_assert(!std::is_move_constructible_v<BlockBuilder>);
static_assert(!std::is_move_assignable_v<BlockBuilder>);
static_assert(BlockCreatableWith<std::vector<std::byte>>);
static_assert(!BlockCreatableWith<std::vector<std::byte>, const ReverseComparator&>);
static_assert(!std::is_constructible_v<Block::Iterator, Block&&, const ReverseComparator&>);
static_assert(!std::is_constructible_v<Block::Iterator, const Block&&, const ReverseComparator&>);
static_assert(!std::is_constructible_v<Block::Iterator, const Block&, ReverseComparator&&>);

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

std::vector<std::byte> ExtendedEntryBytes(std::uint32_t shared, std::string_view delta,
                                          std::string_view value, std::size_t width,
                                          unsigned int terminal_bits) {
  std::vector<std::byte> entry;
  for (const auto length : {shared, static_cast<std::uint32_t>(delta.size()),
                            static_cast<std::uint32_t>(value.size())}) {
    for (std::size_t index = 0; index < width; ++index) {
      unsigned int byte = (length >> (index * 7U)) & 0x7fU;
      byte |= index + 1 < width ? 0x80U : terminal_bits;
      entry.push_back(static_cast<std::byte>(byte));
    }
  }
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

Block MakeBlock(std::vector<std::byte> contents) {
  auto block = Block::Create(std::move(contents));
  EXPECT_TRUE(block.has_value()) << block.error().ToString();
  return std::move(block).value();
}

void ExpectCorruptBlock(std::vector<std::byte> contents) {
  const Result<Block> block = Block::Create(std::move(contents));
  ASSERT_FALSE(block.has_value());
  EXPECT_EQ(block.error().code(), ErrorCode::Corruption);
}

void ExpectInvalidEntries(std::vector<std::byte> contents) {
  const Result<Block> block = Block::Create(std::move(contents));
  ASSERT_TRUE(block.has_value()) << block.error().ToString();
  const Status valid = block->ValidateEntries([](ByteView, ByteView) -> Status { return {}; });
  ASSERT_FALSE(valid.has_value());
  EXPECT_EQ(valid.error().code(), ErrorCode::Corruption);
}

void ExpectOk(const Status& status) {
  ASSERT_TRUE(status.has_value()) << status.error().ToString();
}

void ExpectCorruption(const Status& status) {
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::Corruption);
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

  EXPECT_TRUE(Block::Create(Materialize(builder.Finish())).has_value());
  EXPECT_TRUE(Block::Create(BuildGoldenBlock()).has_value());
}

TEST(BlockTest, PreservesOwnedAndBorrowedStorageAcrossMoves) {
  const std::vector<std::byte> contents = BuildGoldenBlock();
  Result<Block> borrowed = Block::Create(BlockContents::Borrowed(contents));
  ASSERT_TRUE(borrowed.has_value()) << borrowed.error().ToString();
  EXPECT_FALSE(borrowed->cacheable());
  Block moved_borrowed(std::move(*borrowed));
  Block::Iterator borrowed_iterator(moved_borrowed, BytewiseComparator());
  ExpectOk(borrowed_iterator.SeekToFirst());
  ExpectAt(borrowed_iterator, "apple", "1");

  Result<Block> owned = Block::Create(contents);
  ASSERT_TRUE(owned.has_value()) << owned.error().ToString();
  EXPECT_TRUE(owned->cacheable());
  Block moved_owned(std::move(*owned));
  Block::Iterator owned_iterator(moved_owned, BytewiseComparator());
  ExpectOk(owned_iterator.SeekToLast());
  ExpectAt(owned_iterator, "banana", "3");
}

TEST(BlockTest, ReportsWhetherItHasEntries) {
  BlockBuilder builder(16);

  EXPECT_TRUE(MakeBlock(Materialize(builder.Finish())).empty());
  EXPECT_FALSE(MakeBlock(BuildGoldenBlock()).empty());
}

TEST(BlockTest, RejectsBlocksWithoutAValidRestartCount) {
  ExpectCorruptBlock({});
  ExpectCorruptBlock(Bytes({0x00, 0x00, 0x00}));
  ExpectCorruptBlock(Bytes({0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00}));
}

TEST(BlockTest, PhysicalValidationRejectsInvalidRestartPoints) {
  ExpectInvalidEntries(WithRestarts({}, {}));
  ExpectInvalidEntries(WithRestarts({}, {4}));
  ExpectInvalidEntries(WithRestarts({}, {0, 0}));
  ExpectInvalidEntries(WithRestarts(GoldenEntries(), {9, 18}));
  ExpectInvalidEntries(WithRestarts(GoldenEntries(), {0, 19}));
  ExpectInvalidEntries(WithRestarts(GoldenEntries(), {0, 0}));
  ExpectInvalidEntries(WithRestarts(GoldenEntries(), {0, 18, 9}));
  ExpectInvalidEntries(WithRestarts(GoldenEntries(), {0, 18, 28}));
  ExpectInvalidEntries(WithRestarts(GoldenEntries(), {0, 9}));
}

TEST(BlockTest, PhysicalValidationRejectsMalformedEntries) {
  ExpectInvalidEntries(WithRestarts(Bytes({0x80}), {0}));
  ExpectInvalidEntries(WithRestarts(Bytes({0x00}), {0}));
  ExpectInvalidEntries(WithRestarts(Bytes({0x00, 0x05}), {0}));
  ExpectInvalidEntries(WithRestarts(Bytes({0x00, 0x05, 0x01, 'a', 'p'}), {0}));
  ExpectInvalidEntries(WithRestarts(Bytes({0x00, 0x01, 0x80, 'a'}), {0}));
  ExpectInvalidEntries(WithRestarts(Bytes({0xff, 0xff, 0xff, 0xff, 0x7f, 0x00, 0x00}), {0}));
  ExpectInvalidEntries(WithRestarts(Concat({EntryBytes(0, "a", ""), EntryBytes(2, "", "")}), {0}));
}

TEST(BlockTest, PhysicalValidationRejectsUnterminatedExtendedLengths) {
  for (std::size_t field = 0; field < 3; ++field) {
    std::vector<std::byte> entries(field, std::byte{0});
    entries.insert(entries.end(), 5, std::byte{0x80});
    entries.push_back(std::byte{0});
    ExpectInvalidEntries(WithRestarts(std::move(entries), {0}));
  }
}

TEST(BlockTest, PhysicalValidationDoesNotCompareKeyOrder) {
  const std::vector<std::byte> descending =
      WithRestarts(Concat({EntryBytes(0, "b", ""), EntryBytes(0, "a", "")}), {0});
  const std::vector<std::byte> duplicate =
      WithRestarts(Concat({EntryBytes(0, "a", ""), EntryBytes(1, "", "")}), {0});

  for (const auto& contents : {descending, duplicate}) {
    const Result<Block> block = Block::Create(contents);
    ASSERT_TRUE(block.has_value()) << block.error().ToString();
    std::size_t entries = 0;
    const Status valid = block->ValidateEntries([&](ByteView, ByteView) -> Status {
      ++entries;
      return {};
    });
    EXPECT_TRUE(valid.has_value()) << valid.error().ToString();
    EXPECT_EQ(entries, 2U);
  }
}

TEST(BlockIteratorTest, StartsUnpositioned) {
  const Block block = MakeBlock(BuildGoldenBlock());

  const Block::Iterator iterator(block, BytewiseComparator());

  EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, IteratesForwardAndBackward) {
  const Block block = MakeBlock(BuildGoldenBlock());
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectOk(iterator.SeekToFirst());
  ExpectAt(iterator, "apple", "1");
  ExpectOk(iterator.Next());
  ExpectAt(iterator, "applesauce", "2");
  ExpectOk(iterator.Next());
  ExpectAt(iterator, "banana", "3");
  ExpectOk(iterator.Next());
  EXPECT_FALSE(iterator.valid());

  ExpectOk(iterator.SeekToLast());
  ExpectAt(iterator, "banana", "3");
  ExpectOk(iterator.Prev());
  ExpectAt(iterator, "applesauce", "2");
  ExpectOk(iterator.Prev());
  ExpectAt(iterator, "apple", "1");
  ExpectOk(iterator.Prev());
  EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, SeeksToTheFirstKeyNotLessThanTheTarget) {
  const Block block = MakeBlock(BuildGoldenBlock());
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectOk(iterator.Seek(AsBytes("")));
  ExpectAt(iterator, "apple", "1");
  ExpectOk(iterator.Seek(AsBytes("apple")));
  ExpectAt(iterator, "apple", "1");
  ExpectOk(iterator.Seek(AsBytes("applea")));
  ExpectAt(iterator, "applesauce", "2");
  ExpectOk(iterator.Seek(AsBytes("b")));
  ExpectAt(iterator, "banana", "3");
  ExpectOk(iterator.Seek(AsBytes("banana")));
  ExpectAt(iterator, "banana", "3");
  ExpectOk(iterator.Seek(AsBytes("bananas")));
  EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, ChangesDirectionAcrossRestartPoints) {
  const Block block = MakeBlock(BuildGoldenBlock());
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectOk(iterator.Seek(AsBytes("banana")));
  ExpectOk(iterator.Prev());
  ExpectAt(iterator, "applesauce", "2");
  ExpectOk(iterator.Next());
  ExpectAt(iterator, "banana", "3");
  ExpectOk(iterator.Prev());
  ExpectOk(iterator.Prev());
  ExpectAt(iterator, "apple", "1");
  ExpectOk(iterator.Next());
  ExpectAt(iterator, "applesauce", "2");
}

TEST(BlockIteratorTest, HandlesGrowingAndShrinkingKeysAcrossRestarts) {
  const std::array keys{std::string{},        std::string(19, 'a'), std::string(31, 'a'),
                        std::string(32, 'a'), std::string(33, 'a'), std::string(80, 'a'),
                        std::string("b")};
  for (const std::uint32_t restart_interval : {1U, 3U, 16U}) {
    SCOPED_TRACE(restart_interval);
    BlockBuilder builder(restart_interval);
    for (const auto& key : keys) {
      ASSERT_TRUE(builder.Add(AsBytes(key), AsBytes("value")).has_value());
    }
    const Block block = MakeBlock(Materialize(builder.Finish()));
    Block::Iterator iterator(block, BytewiseComparator());
    ExpectOk(iterator.SeekToFirst());
    for (const auto& key : keys) {
      ExpectAt(iterator, key, "value");
      ExpectOk(iterator.Next());
    }
    EXPECT_FALSE(iterator.valid());
    for (const auto& key : keys) {
      ExpectOk(iterator.Seek(AsBytes(key)));
      ExpectAt(iterator, key, "value");
    }
    ExpectOk(iterator.SeekToLast());
    for (std::size_t remaining = keys.size(); remaining > 0; --remaining) {
      ExpectAt(iterator, keys[remaining - 1], "value");
      ExpectOk(iterator.Prev());
    }
    EXPECT_FALSE(iterator.valid());
  }
}

TEST(BlockIteratorTest, ReconstructsBinaryRestartKeys) {
  std::string short_key(19, 'a');
  std::string long_key(33, 'b');
  short_key[1] = long_key[1] = '\0';
  short_key[2] = long_key[2] = '\xff';
  constexpr std::string_view BinaryValue("v\0\xff", 3);
  BlockBuilder builder(1);
  ASSERT_TRUE(builder.Add(AsBytes(short_key), AsBytes(BinaryValue)).has_value());
  ASSERT_TRUE(builder.Add(AsBytes(long_key), AsBytes("last")).has_value());
  const Block block = MakeBlock(Materialize(builder.Finish()));
  Block::Iterator iterator(block, BytewiseComparator());
  ExpectOk(iterator.SeekToLast());
  ExpectAt(iterator, long_key, "last");
  ExpectOk(iterator.Prev());
  ExpectAt(iterator, short_key, BinaryValue);
  ExpectOk(iterator.Next());
  ExpectAt(iterator, long_key, "last");
  ExpectOk(iterator.Seek(AsBytes(short_key)));
  ExpectAt(iterator, short_key, BinaryValue);
}

TEST(BlockIteratorTest, DecodesEveryAcceptedExtendedLengthEncoding) {
  constexpr std::array<std::pair<std::size_t, unsigned int>, 6> Encodings{
      {{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {5, 0x70}}};
  constexpr std::string_view BinaryValue("1\0\xff", 3);
  for (const auto& [width, terminal_bits] : Encodings) {
    SCOPED_TRACE(testing::Message() << width << "/" << terminal_bits);
    const auto first = ExtendedEntryBytes(0, "a", BinaryValue, width, terminal_bits);
    const auto second = ExtendedEntryBytes(1, "b", "", width, terminal_bits);
    const auto restart = static_cast<std::uint32_t>(first.size() + second.size());
    const Block block = MakeBlock(WithRestarts(
        Concat({first, second, ExtendedEntryBytes(0, "c", "last", width, terminal_bits)}),
        {0, restart}));
    Block::Iterator iterator(block, BytewiseComparator());

    ExpectOk(iterator.SeekToFirst());
    ExpectAt(iterator, "a", BinaryValue);
    ExpectOk(iterator.Next());
    ExpectAt(iterator, "ab", "");
    ExpectOk(iterator.Next());
    ExpectAt(iterator, "c", "last");
    ExpectOk(iterator.Next());
    EXPECT_FALSE(iterator.valid());

    ExpectOk(iterator.Seek(AsBytes("ab")));
    ExpectAt(iterator, "ab", "");
    ExpectOk(iterator.Seek(AsBytes("b")));
    ExpectAt(iterator, "c", "last");
    ExpectOk(iterator.Prev());
    ExpectAt(iterator, "ab", "");
    ExpectOk(iterator.Next());
    ExpectAt(iterator, "c", "last");
    ExpectOk(iterator.SeekToLast());
    ExpectAt(iterator, "c", "last");
    ExpectOk(iterator.Prev());
    ExpectOk(iterator.Prev());
    ExpectAt(iterator, "a", BinaryValue);
    ExpectOk(iterator.Prev());
    EXPECT_FALSE(iterator.valid());
  }
}

TEST(BlockIteratorTest, RemainsValidWhenTheBlockMoves) {
  Block block = MakeBlock(BuildGoldenBlock());
  Block::Iterator iterator(block, BytewiseComparator());
  ExpectOk(iterator.SeekToFirst());

  const Block moved(std::move(block));

  ExpectAt(iterator, "apple", "1");
  ExpectOk(iterator.Next());
  ExpectAt(iterator, "applesauce", "2");
  ExpectOk(iterator.SeekToLast());
  ExpectAt(iterator, "banana", "3");
}

TEST(BlockIteratorTest, EmptyBlockHasNoPositions) {
  BlockBuilder builder(16);
  const Block block = MakeBlock(Materialize(builder.Finish()));
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectOk(iterator.SeekToFirst());
  EXPECT_FALSE(iterator.valid());
  ExpectOk(iterator.SeekToLast());
  EXPECT_FALSE(iterator.valid());
  ExpectOk(iterator.Seek(AsBytes("a")));
  EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, ZeroRestartsHaveNoPositionsWithoutDecodingEntryBytes) {
  const Block block = MakeBlock(WithRestarts(Bytes({0x80, 0x80, 0x80}), {}));
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectOk(iterator.SeekToFirst());
  EXPECT_FALSE(iterator.valid());
  ExpectOk(iterator.SeekToLast());
  EXPECT_FALSE(iterator.valid());
  ExpectOk(iterator.Seek(AsBytes("target")));
  EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, ReportsReachedCorruptionAndPositioningRecovers) {
  const auto first = EntryBytes(0, "a", "1");
  const Block block = MakeBlock(WithRestarts(Concat({first, Bytes({0x80})}), {0}));
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectOk(iterator.SeekToFirst());
  ExpectAt(iterator, "a", "1");
  ExpectCorruption(iterator.Next());
  EXPECT_FALSE(iterator.valid());

  ExpectOk(iterator.SeekToFirst());
  ExpectAt(iterator, "a", "1");
  ExpectCorruption(iterator.SeekToLast());
  EXPECT_FALSE(iterator.valid());

  ExpectOk(iterator.Seek(AsBytes("a")));
  ExpectAt(iterator, "a", "1");
}

TEST(BlockIteratorTest, RejectsOutOfRangeRestartOffsetsOnEveryPositioningPath) {
  const auto entry = EntryBytes(0, "a", "");
  const std::uint32_t outside = static_cast<std::uint32_t>(entry.size() + 1);
  const Block block = MakeBlock(WithRestarts(entry, {outside}));
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectCorruption(iterator.SeekToFirst());
  ExpectCorruption(iterator.SeekToLast());
  ExpectCorruption(iterator.Seek(AsBytes("a")));
}

TEST(BlockIteratorTest, SeekRejectsRestartAtTheEntryBoundary) {
  const auto entry = EntryBytes(0, "a", "");
  const std::uint32_t end = static_cast<std::uint32_t>(entry.size());
  const Block block = MakeBlock(WithRestarts(entry, {0, end}));
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectCorruption(iterator.Seek(AsBytes("z")));
}

TEST(BlockIteratorTest, SeekRejectsRestartBeyondTheEntryBoundary) {
  const auto entry = EntryBytes(0, "a", "");
  const std::uint32_t outside = static_cast<std::uint32_t>(entry.size() + 1);
  const Block block = MakeBlock(WithRestarts(entry, {0, outside}));
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectCorruption(iterator.Seek(AsBytes("z")));
}

TEST(BlockIteratorTest, SeekRejectsATruncatedRestartHeader) {
  const auto first = EntryBytes(0, "a", "");
  std::vector<std::byte> entries = first;
  entries.insert(entries.end(), 3, std::byte{0x80});
  const Block block =
      MakeBlock(WithRestarts(std::move(entries), {0, static_cast<std::uint32_t>(first.size())}));
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectCorruption(iterator.Seek(AsBytes("z")));
}

TEST(BlockIteratorTest, SeekRejectsAMalformedRestartEntryAndRecovers) {
  const auto first = EntryBytes(0, "a", "");
  const auto malformed_restart = EntryBytes(1, "z", "");
  const Block block = MakeBlock(WithRestarts(Concat({first, malformed_restart}),
                                             {0, static_cast<std::uint32_t>(first.size())}));
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectCorruption(iterator.Seek(AsBytes("z")));
  EXPECT_FALSE(iterator.valid());
  ExpectOk(iterator.SeekToFirst());
  ExpectAt(iterator, "a", "");
}

TEST(BlockIteratorTest, PrevReportsCorruptionBeforeThePreviousEntryAndRecovers) {
  const auto first = EntryBytes(0, "a", "");
  const auto malformed = EntryBytes(2, "", "");
  const auto last = EntryBytes(0, "c", "");
  const std::uint32_t last_offset = static_cast<std::uint32_t>(first.size() + malformed.size());
  const Block block = MakeBlock(WithRestarts(Concat({first, malformed, last}), {0, last_offset}));
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectOk(iterator.SeekToLast());
  ExpectAt(iterator, "c", "");
  ExpectCorruption(iterator.Prev());
  EXPECT_FALSE(iterator.valid());
  ExpectOk(iterator.SeekToLast());
  ExpectAt(iterator, "c", "");
}

TEST(BlockIteratorTest, PrevRejectsAnEarlierOutOfRangeRestart) {
  const auto entry = EntryBytes(0, "a", "");
  const std::uint32_t outside = static_cast<std::uint32_t>(entry.size() + 1);
  const Block block = MakeBlock(WithRestarts(entry, {outside, 0}));
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectOk(iterator.SeekToLast());
  ExpectAt(iterator, "a", "");
  ExpectCorruption(iterator.Prev());
}

TEST(BlockIteratorTest, ForwardDecodeRejectsALaterOutOfRangeRestart) {
  const auto entry = EntryBytes(0, "a", "");
  const std::uint32_t outside = static_cast<std::uint32_t>(entry.size() + 1);
  const Block block = MakeBlock(WithRestarts(entry, {0, outside}));
  Block::Iterator iterator(block, BytewiseComparator());

  ExpectCorruption(iterator.SeekToFirst());
}

TEST(BlockIteratorTest, InternalKeyModeRejectsShortTargetsAndEntries) {
  const InternalKeyComparator defensive(BytewiseComparator());
  const TrustedInternalKeyComparator trusted(defensive);
  const Block block = MakeBlock(WithRestarts(EntryBytes(0, "short", ""), {0}));
  Block::Iterator iterator(block, trusted, BlockKeyFormat::Internal);

  ExpectCorruption(iterator.Seek(AsBytes("short")));
  EXPECT_FALSE(iterator.valid());
  ExpectCorruption(iterator.SeekToFirst());
  EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, InternalKeySeekRejectsAShortRestartKey) {
  const InternalKeyComparator defensive(BytewiseComparator());
  const TrustedInternalKeyComparator trusted(defensive);
  const InternalKey valid = InternalKey::Create(AsBytes("a"), 1, ValueKind::Value).value();
  const auto first = EntryBytes(0, AsStringView(valid.encoded()), "");
  const auto short_restart = EntryBytes(0, "short", "");
  const Block block = MakeBlock(
      WithRestarts(Concat({first, short_restart}), {0, static_cast<std::uint32_t>(first.size())}));
  Block::Iterator iterator(block, trusted, BlockKeyFormat::Internal);
  const InternalKey target = InternalKey::Create(AsBytes("z"), 1, ValueKind::Value).value();

  ExpectCorruption(iterator.Seek(target.encoded()));
}

TEST(BlockIteratorTest, UsesTheBlockComparator) {
  const ReverseComparator reverse;
  BlockBuilder builder(1);
  ASSERT_TRUE(builder.Add(AsBytes("c"), AsBytes("3")).has_value());
  ASSERT_TRUE(builder.Add(AsBytes("b"), AsBytes("2")).has_value());
  ASSERT_TRUE(builder.Add(AsBytes("a"), AsBytes("1")).has_value());
  const Block block = MakeBlock(Materialize(builder.Finish()));
  Block::Iterator iterator(block, reverse);

  ExpectOk(iterator.Seek(AsBytes("bb")));
  ExpectAt(iterator, "b", "2");
  ExpectOk(iterator.Seek(AsBytes("d")));
  ExpectAt(iterator, "c", "3");
  ExpectOk(iterator.Seek(AsBytes("")));
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
    const Block block = MakeBlock(contents);
    Block::Iterator iterator(block, comparator);

    ExpectOk(iterator.SeekToFirst());
    for (std::size_t index = 0; index <= entries.size(); ++index) {
      ExpectAtEntry(iterator, entries, index);
      if (iterator.valid()) {
        ExpectOk(iterator.Next());
      }
    }

    ExpectOk(iterator.SeekToLast());
    for (std::size_t remaining = entries.size(); remaining > 0; --remaining) {
      ExpectAtEntry(iterator, entries, remaining - 1);
      ExpectOk(iterator.Prev());
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
      ExpectOk(iterator.Seek(target));
      ExpectAtEntry(iterator, entries, index);

      for (int step = 0; step < 8 && index < entries.size(); ++step) {
        if (Below(2) == 0) {
          ExpectOk(iterator.Next());
          ++index;
        } else if (index == 0) {
          ExpectOk(iterator.Prev());
          index = entries.size();
        } else {
          ExpectOk(iterator.Prev());
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
