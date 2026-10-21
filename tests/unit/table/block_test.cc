#include "table/block.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
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
#include "table/compression.h"

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

class RecordingComparator final : public Comparator {
public:
    int Compare(ByteView left, ByteView right) const noexcept override {
        comparisons.emplace_back(AsStringView(left), AsStringView(right));
        return BytewiseComparator().Compare(left, right);
    }
    std::string_view Name() const noexcept override { return "test.RecordingComparator"; }
    void FindShortestSeparator(std::vector<std::byte>&, ByteView) const override {}
    void FindShortSuccessor(std::vector<std::byte>&) const override {}

    mutable std::vector<std::pair<std::string, std::string>> comparisons;
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

// Each of the first two entries is three one-byte lengths + five suffix bytes
// + one value byte = 9 bytes, so the next full-key restart starts at offset 18.
std::vector<std::byte> GoldenEntries() {
    return Concat({
        EntryBytes(0, "apple", "1"),
        EntryBytes(5, "sauce", "2"),
        EntryBytes(0, "banana", "3"),
    });
}

std::vector<std::byte> BuildGoldenBlock() {
    BlockBuilder builder(2);
    builder.Add(AsBytes("apple"), AsBytes("1"));
    builder.Add(AsBytes("applesauce"), AsBytes("2"));
    builder.Add(AsBytes("banana"), AsBytes("3"));
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

TEST(BlockTest, LiteralHeaderPathsKeepBorrowedValuesAcrossBackingMoves) {
    const std::array entries{
        Bytes({0, 1, 1, 'a', 0xff}),
        Bytes({0x80, 0, 0x81, 0, 0x81, 0, 'a', 0xff}),
        Bytes({0x80, 0x80, 0x80, 0x80, 0x10, 0x81, 0x80, 0x80, 0x80, 0x10, 0x81, 0x80, 0x80, 0x80,
               0x10, 'a', 0xff}),
    };
    for (const auto& entry : entries) {
        const auto bytes = Concat({entry, Bytes({0, 0, 0, 0, 1, 0, 0, 0})});
        for (const bool owned : {false, true}) {
            SCOPED_TRACE(entry.size());
            SCOPED_TRACE(owned);
            auto block =
                owned ? Block::Create(bytes) : Block::Create(BlockContents::Borrowed(bytes));
            ASSERT_TRUE(block.has_value());
            EXPECT_EQ(block->cacheable(), owned);
            ByteView retained_value;
            std::size_t visits = 0;
            const Status validated =
                block->ValidateEntries([&](ByteView key, ByteView value) -> Status {
                    ++visits;
                    EXPECT_EQ(AsStringView(key), "a");
                    EXPECT_EQ(Materialize(value), Bytes({0xff}));
                    retained_value = value;
                    return {};
                });
            ASSERT_TRUE(validated.has_value());
            EXPECT_EQ(visits, 1U);
            if (!owned) {
                EXPECT_EQ(retained_value.data(), bytes.data() + entry.size() - 1);
            }

            Block moved(std::move(*block));
            Block::Iterator iterator(moved, BytewiseComparator());
            ASSERT_TRUE(iterator.SeekToFirst().has_value());
            ASSERT_TRUE(iterator.valid());
            EXPECT_EQ(AsStringView(iterator.key()), "a");
            EXPECT_EQ(iterator.key().data(), retained_value.data() - 1);
            EXPECT_EQ(iterator.value().data(), retained_value.data());
            EXPECT_EQ(Materialize(retained_value), Bytes({0xff}));
            EXPECT_TRUE(iterator.Next().has_value());
            EXPECT_FALSE(iterator.valid());
        }
    }
}

TEST(BlockTest, LiteralHeaderAndPayloadFailuresDoNotPublishVisitorEntries) {
    const auto complete = Bytes({0x80, 0x80, 0x80, 0x80, 0x10, 0x81, 0x80, 0x80, 0x80, 0x10, 0x81,
                                 0x80, 0x80, 0x80, 0x10, 'a', 0xff});
    std::vector<std::vector<std::byte>> malformed{
        Bytes({0, 1, 1}),
        Bytes({0, 1, 1, 'a'}),
        Bytes({0, 0xff, 0xff, 0xff, 0xff, 0x0f, 1}),
        Bytes({0, 1, 0xff, 0xff, 0xff, 0xff, 0x0f}),
        Bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0, 1, 1, 'a', 0xff}),
    };
    for (std::size_t size = 1; size < complete.size(); ++size) {
        malformed.emplace_back(complete.begin(),
                               complete.begin() + static_cast<std::ptrdiff_t>(size));
    }
    for (const auto& entry : malformed) {
        SCOPED_TRACE(entry.size());
        auto block = Block::Create(Concat({entry, Bytes({0, 0, 0, 0, 1, 0, 0, 0})}));
        ASSERT_TRUE(block.has_value());
        std::size_t visits = 0;
        const Status validated = block->ValidateEntries([&](ByteView, ByteView) -> Status {
            ++visits;
            return {};
        });
        ASSERT_FALSE(validated.has_value());
        EXPECT_EQ(validated.error().code(), ErrorCode::Corruption);
        EXPECT_EQ(validated.error().message(), "block entry is malformed");
        EXPECT_EQ(visits, 0U);
    }
}

TEST(BlockTest, LiteralFooterAdmissionKeepsPriorityAndLazyZeroCountTopology) {
    const auto footer = Bytes({1, 0, 0, 0});
    for (std::size_t size = 0; size < footer.size(); ++size) {
        const auto block = Block::Create(std::vector<std::byte>(
            footer.begin(), footer.begin() + static_cast<std::ptrdiff_t>(size)));
        ASSERT_FALSE(block.has_value());
        EXPECT_EQ(block.error().code(), ErrorCode::Corruption);
        EXPECT_EQ(block.error().message(), "block is too short");
        EXPECT_TRUE(std::string_view(block.error().location().file_name()).ends_with("block.cc"));
        EXPECT_NE(std::string_view(block.error().location().function_name()).find("Create"),
                  std::string_view::npos);
    }
    for (const auto& bytes : {footer, Bytes({0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff})}) {
        const auto block = Block::Create(bytes);
        ASSERT_FALSE(block.has_value());
        EXPECT_EQ(block.error().code(), ErrorCode::Corruption);
        EXPECT_EQ(block.error().message(), "block restart count is invalid");
        EXPECT_NE(std::string_view(block.error().location().function_name()).find("Create"),
                  std::string_view::npos);
    }
    for (const auto& bytes : {Bytes({0, 0, 0, 0}), Bytes({0xff, 0, 0, 0, 0})}) {
        const auto block = Block::Create(bytes);
        ASSERT_TRUE(block.has_value());
        EXPECT_TRUE(block->empty());
        std::size_t visits = 0;
        const auto validated = block->ValidateEntries([&](ByteView, ByteView) -> Status {
            ++visits;
            return {};
        });
        ASSERT_FALSE(validated.has_value());
        EXPECT_EQ(validated.error().message(), "block restart points are invalid");
        EXPECT_EQ(visits, 0U);
    }
    const auto canonical_empty = Block::Create(Bytes({0, 0, 0, 0, 1, 0, 0, 0}));
    ASSERT_TRUE(canonical_empty.has_value());
    EXPECT_TRUE(canonical_empty->empty());
    EXPECT_TRUE(canonical_empty
                    ->ValidateEntries([](ByteView, ByteView) -> Status {
                        ADD_FAILURE() << "An empty block must not visit entries";
                        return {};
                    })
                    .has_value());
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
    builder.Add(AsBytes("key1"), AsBytes("a"));
    builder.Add(AsBytes("key2"), AsBytes("b"));

    EXPECT_EQ(
        Materialize(builder.Finish()),
        WithRestarts(Concat({EntryBytes(0, "key1", "a"), EntryBytes(0, "key2", "b")}), {0, 8}));
}

TEST(BlockBuilderTest, EstimatesTheFinishedSize) {
    BlockBuilder builder(2);
    builder.Add(AsBytes("apple"), AsBytes("1"));
    builder.Add(AsBytes("applesauce"), AsBytes("2"));
    builder.Add(AsBytes("banana"), AsBytes("3"));

    const std::size_t estimate = builder.CurrentSizeEstimate();

    EXPECT_FALSE(builder.empty());
    EXPECT_EQ(builder.Finish().size(), estimate);
}

TEST(BlockBuilderTest, ResetStartsANewBlock) {
    BlockBuilder builder(2);
    builder.Add(AsBytes("zebra"), AsBytes("z"));
    (void)builder.Finish();

    builder.Reset();

    EXPECT_TRUE(builder.empty());
    EXPECT_EQ(builder.CurrentSizeEstimate(), 8U);
    builder.Add(AsBytes("apple"), AsBytes("1"));
    builder.Add(AsBytes("applesauce"), AsBytes("2"));
    builder.Add(AsBytes("banana"), AsBytes("3"));
    EXPECT_EQ(Materialize(builder.Finish()), WithRestarts(GoldenEntries(), {0, 18}));
}

TEST(BlockBuilderTest, BinaryPrefixAndRestartStagesKeepLiteralBlocks) {
    struct BuilderCase {
        std::uint32_t interval;
        std::vector<std::byte> expected;
    };
    const std::array cases{
        BuilderCase{1,
                    Bytes({0, 0, 1, 0xff, 0, 1, 2, 0, 0,  0xff, 0, 2, 0,  0, 0xff, 0, 1, 1, 1, 0x7f,
                           0, 0, 0, 0,    4, 0, 0, 0, 10, 0,    0, 0, 15, 0, 0,    0, 4, 0, 0, 0})},
        BuilderCase{2, Bytes({0, 0, 1, 0xff, 0, 1, 2, 0, 0,  0xff, 0, 2, 0, 0, 0xff, 0,
                              1, 1, 1, 0x7f, 0, 0, 0, 0, 10, 0,    0, 0, 2, 0, 0,    0})},
        BuilderCase{3, Bytes({0, 0, 1,    0xff, 0, 1, 2, 0,  0, 0xff, 1, 1, 0, 0xff, 0, 1,
                              1, 1, 0x7f, 0,    0, 0, 0, 14, 0, 0,    0, 2, 0, 0,    0})},
    };
    const std::array keys{std::vector<std::byte>{}, Bytes({0}), Bytes({0, 0xff}), Bytes({1})};
    const std::array values{Bytes({0xff}), Bytes({0, 0xff}), std::vector<std::byte>{},
                            Bytes({0x7f})};
    for (const auto& test : cases) {
        SCOPED_TRACE(test.interval);
        BlockBuilder builder(test.interval);
        for (std::size_t index = 0; index < keys.size(); ++index) {
            builder.Add(keys[index], values[index]);
        }
        EXPECT_FALSE(builder.empty());
        EXPECT_EQ(builder.CurrentSizeEstimate(), test.expected.size());
        EXPECT_EQ(Materialize(builder.Finish()), test.expected);
    }
}

TEST(BlockBuilderTest, ResetAfterOwnedFinishRestoresLiteralPrefixHistory) {
    BlockBuilder builder(2);
    builder.Add(Bytes({0}), Bytes({0xff}));
    builder.Add(Bytes({0, 0xff}), {});
    const auto first = Materialize(builder.Finish());
    const auto first_expected = Bytes({0, 1, 1, 0, 0xff, 1, 1, 0, 0xff, 0, 0, 0, 0, 1, 0, 0, 0});
    EXPECT_EQ(first, first_expected);

    builder.Reset();
    EXPECT_TRUE(builder.empty());
    EXPECT_EQ(builder.CurrentSizeEstimate(), 8U);
    builder.Add(Bytes({0, 0xff}), Bytes({0x80}));
    const auto expected = Bytes({0, 2, 1, 0, 0xff, 0x80, 0, 0, 0, 0, 1, 0, 0, 0});
    EXPECT_FALSE(builder.empty());
    EXPECT_EQ(builder.CurrentSizeEstimate(), expected.size());
    EXPECT_EQ(Materialize(builder.Finish()), expected);
    EXPECT_EQ(first, first_expected);
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
    ExpectInvalidEntries(
        WithRestarts(Concat({EntryBytes(0, "a", ""), EntryBytes(2, "", "")}), {0}));
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

TEST(BlockTest, PhysicalValidationKeepsDecodeRestartAndVisitorFailurePriority) {
    const auto first = EntryBytes(0, "a", "1");
    const std::uint32_t second_offset = static_cast<std::uint32_t>(first.size());
    for (const bool malformed : {true, false}) {
        SCOPED_TRACE(malformed);
        const auto second = malformed ? EntryBytes(2, "", "") : EntryBytes(1, "b", "2");
        const Block block = MakeBlock(WithRestarts(Concat({first, second}), {0, second_offset}));
        std::vector<std::string> visited;
        const Status result = block.ValidateEntries([&](ByteView key, ByteView) -> Status {
            visited.emplace_back(AsStringView(key));
            return {};
        });
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::Corruption);
        EXPECT_EQ(result.error().message(),
                  malformed ? "block entry is malformed" : "block restart points are invalid");
        EXPECT_EQ(visited, std::vector<std::string>{"a"});
    }
}

TEST(BlockTest, VisitorFailureStopsBeforeLaterEntriesAndFinalRestartValidation) {
    const auto first = EntryBytes(0, "b", "1");
    for (const auto& later : {Bytes({0x80}), EntryBytes(0, "a", "2")}) {
        const Block block = MakeBlock(WithRestarts(Concat({first, later}), {0, 0}));
        std::vector<std::pair<std::string, std::string>> visited;
        const Status result = block.ValidateEntries([&](ByteView key, ByteView value) -> Status {
            visited.emplace_back(AsStringView(key), AsStringView(value));
            return std::unexpected(Error::Io("visitor stopped"));
        });
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::Io);
        EXPECT_EQ(result.error().message(), "visitor stopped");
        EXPECT_EQ(visited, (std::vector<std::pair<std::string, std::string>>{{"b", "1"}}));
    }
}

TEST(BlockIteratorTest, StartsUnpositioned) {
    const Block block = MakeBlock(BuildGoldenBlock());

    const Block::Iterator iterator(block, BytewiseComparator());

    EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, CompleteKeysBorrowEncodedBytesAcrossPositioningAndBackingMoves) {
    const std::array keys{std::string{}, std::string(19, 'a'), std::string(128, 'a'),
                          std::string(512, 'a'), std::string("b\0\xff", 3)};
    BlockBuilder builder(1);
    for (const auto& key : keys) {
        builder.Add(AsBytes(key), AsBytes("value"));
    }
    const auto finished = Materialize(builder.Finish());

    for (const bool owned : {false, true}) {
        SCOPED_TRACE(owned);
        auto encoded = finished;
        auto block = Block::Create(owned ? BlockContents::Owned(std::move(encoded))
                                         : BlockContents::Borrowed(encoded));
        ASSERT_TRUE(block.has_value());
        std::optional<Block> moved;
        Block::Iterator iterator(*block, BytewiseComparator());
        ExpectOk(iterator.SeekToFirst());
        const std::byte* first_key = iterator.key().data();
        moved.emplace(std::move(*block));
        EXPECT_EQ(iterator.key().data(), first_key);

        for (const auto& key : keys) {
            ExpectAt(iterator, key, "value");
            EXPECT_EQ(iterator.key().data(), iterator.value().data() - key.size());
            ExpectOk(iterator.Next());
        }
        EXPECT_FALSE(iterator.valid());
        EXPECT_TRUE(iterator.key().empty());

        for (const auto& key : keys) {
            ExpectOk(iterator.Seek(AsBytes(key)));
            ExpectAt(iterator, key, "value");
            EXPECT_EQ(iterator.key().data(), iterator.value().data() - key.size());
        }

        ExpectOk(iterator.SeekToLast());
        for (std::size_t remaining = keys.size(); remaining > 0; --remaining) {
            const auto& key = keys[remaining - 1];
            ExpectAt(iterator, key, "value");
            EXPECT_EQ(iterator.key().data(), iterator.value().data() - key.size());
            ExpectOk(iterator.Prev());
        }
        EXPECT_FALSE(iterator.valid());
        EXPECT_TRUE(iterator.key().empty());
    }
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

TEST(BlockIteratorTest, SeekReusesOnlyTheCurrentForwardCheckpoint) {
    const Block block =
        MakeBlock(WithRestarts(Concat({EntryBytes(0, "a", "1"), EntryBytes(0, "b", "2"),
                                       EntryBytes(0, "c", "3"), EntryBytes(0, "d", "4")}),
                               {0}));
    const RecordingComparator comparator;
    Block::Iterator iterator(block, comparator);
    using Comparisons = std::vector<std::pair<std::string, std::string>>;

    ExpectOk(iterator.Seek(AsBytes("b")));
    ExpectAt(iterator, "b", "2");
    EXPECT_EQ(comparator.comparisons, (Comparisons{{"a", "b"}, {"b", "b"}}));
    comparator.comparisons.clear();

    ExpectOk(iterator.Seek(AsBytes("d")));
    ExpectAt(iterator, "d", "4");
    EXPECT_EQ(comparator.comparisons, (Comparisons{{"b", "d"}, {"c", "d"}, {"d", "d"}}));
    comparator.comparisons.clear();

    ExpectOk(iterator.Seek(AsBytes("d")));
    ExpectAt(iterator, "d", "4");
    EXPECT_EQ(comparator.comparisons, (Comparisons{{"d", "d"}}));
    comparator.comparisons.clear();

    ExpectOk(iterator.Seek(AsBytes("b")));
    ExpectAt(iterator, "b", "2");
    EXPECT_EQ(comparator.comparisons, (Comparisons{{"d", "b"}, {"a", "b"}, {"b", "b"}}));
}

TEST(BlockIteratorTest, ReverseEmptyAndBinaryEntriesKeepExactOwnedObservationsWithoutComparison) {
    constexpr std::string_view BinaryPrefix("a\0", 2);
    constexpr std::string_view BinarySuffix("\xff", 1);
    constexpr std::string_view BinaryValue("v\0\xff", 3);
    const auto first = EntryBytes(0, "", "zero");
    const auto second = EntryBytes(0, BinaryPrefix, "first");
    const auto third = EntryBytes(2, BinarySuffix, BinaryValue);
    const auto last = EntryBytes(0, "z", "last");
    const auto last_offset =
        static_cast<std::uint32_t>(first.size() + second.size() + third.size());
    const Block block =
        MakeBlock(WithRestarts(Concat({first, second, third, last}), {0, last_offset}));
    const RecordingComparator comparator;
    Block::Iterator iterator(block, comparator);
    const std::vector<std::pair<std::string, std::string>> expected{
        {"z", "last"},
        {std::string("a\0\xff", 3), std::string("v\0\xff", 3)},
        {std::string("a\0", 2), "first"},
        {"", "zero"},
    };

    for (int pass = 0; pass < 2; ++pass) {
        SCOPED_TRACE(pass);
        ExpectOk(iterator.SeekToLast());
        std::vector<std::pair<std::string, std::string>> observed;
        for (std::size_t entry = 0; entry < expected.size(); ++entry) {
            ASSERT_TRUE(iterator.valid());
            observed.emplace_back(AsStringView(iterator.key()), AsStringView(iterator.value()));
            ExpectOk(iterator.Prev());
        }
        EXPECT_EQ(observed, expected);
        EXPECT_FALSE(iterator.valid());
        EXPECT_TRUE(iterator.key().empty());
        EXPECT_TRUE(iterator.value().empty());
    }
    ExpectOk(iterator.SeekToFirst());
    ExpectAt(iterator, "", "zero");
    ExpectOk(iterator.Next());
    ExpectAt(iterator, BinaryPrefix, "first");
    ExpectOk(iterator.Prev());
    ExpectAt(iterator, "", "zero");
    ExpectOk(iterator.Next());
    ExpectAt(iterator, BinaryPrefix, "first");
    EXPECT_TRUE(comparator.comparisons.empty());
}

TEST(BlockIteratorTest, ReverseTraversalBorrowsDistinctCachedDecodedKeys) {
    const std::string prefix = std::string(80, 'a') + std::string("\0\xff", 2);
    const std::array keys{prefix + "1", prefix + "2", prefix + "3", std::string("z")};
    BlockBuilder builder(4);
    for (std::size_t index = 0; index < keys.size(); ++index) {
        builder.Add(AsBytes(keys[index]), AsBytes(std::to_string(index + 1)));
    }
    auto block = Block::Create(Materialize(builder.Finish()));
    ASSERT_TRUE(block.has_value());
    std::optional<Block> moved;
    Block::Iterator iterator(*block, BytewiseComparator());

    ExpectOk(iterator.SeekToLast());
    ExpectAt(iterator, keys[3], "4");
    ExpectOk(iterator.Prev());
    ExpectAt(iterator, keys[2], "3");
    const std::byte* third_key = iterator.key().data();
    ExpectOk(iterator.Prev());
    ExpectAt(iterator, keys[1], "2");
    const std::byte* second_key = iterator.key().data();
    EXPECT_NE(second_key, third_key);

    moved.emplace(std::move(*block));
    EXPECT_EQ(iterator.key().data(), second_key);
    ExpectAt(iterator, keys[1], "2");
    ExpectOk(iterator.Next());
    ExpectAt(iterator, keys[2], "3");
    ExpectOk(iterator.Prev());
    ExpectAt(iterator, keys[1], "2");
    ExpectOk(iterator.Prev());
    ExpectAt(iterator, keys[0], "1");
    EXPECT_EQ(iterator.key().data(), iterator.value().data() - keys[0].size());
    ExpectOk(iterator.Prev());
    EXPECT_FALSE(iterator.valid());
    EXPECT_TRUE(iterator.key().empty());
    EXPECT_TRUE(iterator.value().empty());
    ExpectOk(iterator.SeekToLast());
    ExpectOk(iterator.Prev());
    ExpectAt(iterator, keys[2], "3");
}

TEST(BlockIteratorTest, RepositioningDoesNotReuseADifferentRestartReplay) {
    const auto fake = EntryBytes(0, "a", "fake");
    const auto first = EntryBytes(0, "b", AsStringView(fake));
    const auto merged = EntryBytes(1, "2", "actual");
    const auto last = EntryBytes(0, "z", "last");
    const auto fake_offset = static_cast<std::uint32_t>(first.size() - fake.size());
    const auto last_offset = static_cast<std::uint32_t>(first.size() + merged.size());
    // The inner restart and the physical path share an offset, not a key history.
    const Block block = MakeBlock(
        WithRestarts(Concat({first, merged, last}), {0, last_offset, fake_offset, last_offset}));
    Block::Iterator iterator(block, BytewiseComparator());

    ExpectOk(iterator.SeekToLast());
    ExpectAt(iterator, "z", "last");
    ExpectOk(iterator.Prev());
    ExpectAt(iterator, "a2", "actual");
    ExpectOk(iterator.SeekToFirst());
    ExpectAt(iterator, "b", AsStringView(fake));
    ExpectOk(iterator.Next());
    ExpectAt(iterator, "b2", "actual");
    ExpectOk(iterator.Prev());
    ExpectAt(iterator, "b", AsStringView(fake));
}

TEST(BlockIteratorTest, ReverseReplayDeclinesAmplifiedDecodedKeyStorage) {
    const std::string prefix(8192, 'a');
    BlockBuilder builder(16);
    for (int entry = 1; entry <= 8; ++entry) {
        builder.Add(AsBytes(prefix + std::to_string(entry)), AsBytes("v"));
    }
    builder.Add(AsBytes("z"), AsBytes("last"));
    const Block block = MakeBlock(Materialize(builder.Finish()));
    Block::Iterator iterator(block, BytewiseComparator());
    ExpectOk(iterator.SeekToLast());
    ExpectOk(iterator.Prev());
    ExpectAt(iterator, prefix + "8", "v");
    const std::byte* scratch = iterator.key().data();
    ExpectOk(iterator.Prev());
    ExpectAt(iterator, prefix + "7", "v");
    EXPECT_EQ(iterator.key().data(), scratch);
    for (int entry = 6; entry > 0; --entry) {
        ExpectOk(iterator.Prev());
        ExpectAt(iterator, prefix + std::to_string(entry), "v");
    }
    ExpectOk(iterator.Prev());
    EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, DenseReverseReplayKeepsEveryEmptyKeyAndBorrowedValue) {
    BlockBuilder builder(256);
    for (unsigned int entry = 0; entry < 256; ++entry) {
        const std::array value{static_cast<std::byte>(entry)};
        builder.Add({}, value);
    }
    const Block block = MakeBlock(Materialize(builder.Finish()));
    Block::Iterator iterator(block, BytewiseComparator());
    ExpectOk(iterator.SeekToLast());
    for (unsigned int remaining = 256; remaining > 0; --remaining) {
        ASSERT_TRUE(iterator.valid());
        EXPECT_TRUE(iterator.key().empty());
        ASSERT_EQ(iterator.value().size(), 1U);
        EXPECT_EQ(iterator.value()[0], static_cast<std::byte>(remaining - 1));
        EXPECT_EQ(iterator.key().data(), iterator.value().data());
        ExpectOk(iterator.Prev());
    }
    EXPECT_FALSE(iterator.valid());
    EXPECT_TRUE(iterator.key().empty());
    EXPECT_TRUE(iterator.value().empty());
}

TEST(BlockIteratorTest, ReverseCacheDoesNotHideEarlierCorruptionAfterRepositioning) {
    const std::string prefix(64, 'm');
    const auto malformed = EntryBytes(1, "", "");
    const auto first = EntryBytes(0, prefix + "1", "1");
    const auto second = EntryBytes(64, "2", "2");
    const auto third = EntryBytes(64, "3", "3");
    const auto last = EntryBytes(0, "z", "last");
    const auto first_offset = static_cast<std::uint32_t>(malformed.size());
    const auto last_offset =
        static_cast<std::uint32_t>(malformed.size() + first.size() + second.size() + third.size());
    const Block block = MakeBlock(WithRestarts(Concat({malformed, first, second, third, last}),
                                               {0, first_offset, last_offset}));
    Block::Iterator iterator(block, BytewiseComparator());

    for (int pass = 0; pass < 2; ++pass) {
        SCOPED_TRACE(pass);
        ExpectOk(iterator.SeekToLast());
        ExpectAt(iterator, "z", "last");
        for (int entry = 3; entry > 0; --entry) {
            ExpectOk(iterator.Prev());
            ExpectAt(iterator, prefix + std::to_string(entry), std::to_string(entry));
        }
        ExpectCorruption(iterator.Prev());
        EXPECT_FALSE(iterator.valid());
        EXPECT_TRUE(iterator.key().empty());
        EXPECT_TRUE(iterator.value().empty());
    }
}

TEST(BlockIteratorTest, ReverseFailuresClearViewsAndSupportedPositioningRestoresReachableEntries) {
    const auto first = EntryBytes(0, "a", "1");
    const auto malformed = EntryBytes(2, "", "");
    const auto last = EntryBytes(0, "z", "last");
    const auto last_offset = static_cast<std::uint32_t>(first.size() + malformed.size());
    struct Case {
        std::vector<std::byte> contents;
        bool predecessor;
        std::string_view restored_key;
        std::string_view restored_value;
    };
    const std::vector<Case> cases{
        {WithRestarts(Concat({first, malformed, last}), {0, last_offset}), true, "z", "last"},
        {WithRestarts(last, {static_cast<std::uint32_t>(last.size() + 1), 0}), true, "z", "last"},
        {WithRestarts(Concat({first, Bytes({0x80})}), {0}), false, "a", "1"},
    };
    for (const Case& fixture : cases) {
        SCOPED_TRACE(fixture.contents.size());
        const Block block = MakeBlock(fixture.contents);
        const RecordingComparator comparator;
        Block::Iterator iterator(block, comparator);
        for (int pass = 0; pass < 2; ++pass) {
            SCOPED_TRACE(pass);
            Status failed;
            if (fixture.predecessor) {
                ExpectOk(iterator.SeekToLast());
                ExpectAt(iterator, "z", "last");
                failed = iterator.Prev();
            } else {
                failed = iterator.SeekToLast();
            }
            ASSERT_FALSE(failed.has_value());
            EXPECT_EQ(failed.error().code(), ErrorCode::Corruption);
            EXPECT_EQ(failed.error().message(), "bad entry in block");
            EXPECT_FALSE(iterator.valid());
            EXPECT_TRUE(iterator.key().empty());
            EXPECT_TRUE(iterator.value().empty());
            if (fixture.predecessor) {
                ExpectOk(iterator.SeekToLast());
            } else {
                ExpectOk(iterator.SeekToFirst());
            }
            ExpectAt(iterator, fixture.restored_key, fixture.restored_value);
        }
        EXPECT_TRUE(comparator.comparisons.empty());
    }
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
            builder.Add(AsBytes(key), AsBytes("value"));
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
    builder.Add(AsBytes(short_key), AsBytes(BinaryValue));
    builder.Add(AsBytes(long_key), AsBytes("last"));
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

TEST(BlockIteratorTest, DecodesIndependentSingleAndExtendedHeaderFields) {
    constexpr std::array<unsigned int, 3> Lengths{0, 1, 2};
    constexpr std::string_view Value("v\0", 2);
    for (unsigned int extended = 0; extended < 8; ++extended) {
        SCOPED_TRACE(extended);
        std::vector<std::byte> entry;
        for (std::size_t field = 0; field < Lengths.size(); ++field) {
            const bool wide = (extended & (1U << field)) != 0;
            entry.push_back(static_cast<std::byte>(Lengths[field] | (wide ? 0x80U : 0U)));
            if (wide) {
                entry.push_back(std::byte{0});
            }
        }
        entry.push_back(std::byte{'a'});
        entry.insert(entry.end(), AsBytes(Value).begin(), AsBytes(Value).end());
        const Block block = MakeBlock(WithRestarts(entry, {0}));
        Block::Iterator iterator(block, BytewiseComparator());

        ExpectOk(iterator.SeekToFirst());
        ExpectAt(iterator, "a", Value);
        ExpectOk(iterator.Seek(AsBytes("a")));
        ExpectAt(iterator, "a", Value);
        ExpectOk(iterator.SeekToLast());
        ExpectAt(iterator, "a", Value);
        ExpectOk(iterator.Next());
        EXPECT_FALSE(iterator.valid());

        const Block damaged = MakeBlock(WithRestarts(Concat({entry, Bytes({0x80})}), {0}));
        Block::Iterator failing(damaged, BytewiseComparator());
        ExpectOk(failing.SeekToFirst());
        ExpectAt(failing, "a", Value);
        ExpectCorruption(failing.Next());
        EXPECT_FALSE(failing.valid());
        EXPECT_TRUE(failing.key().empty());
        EXPECT_TRUE(failing.value().empty());
        ExpectOk(failing.Seek(AsBytes("a")));
        ExpectAt(failing, "a", Value);
    }
}

TEST(BlockIteratorTest, CorruptionAndRecoveryRemainLocalToEachIterator) {
    const auto first = EntryBytes(0, "a", "first");
    const auto malformed = EntryBytes(2, "", "");
    const auto last = EntryBytes(0, "z", "last");
    const auto restart = static_cast<std::uint32_t>(first.size() + malformed.size());
    const Block block = MakeBlock(WithRestarts(Concat({first, malformed, last}), {0, restart}));
    Block::Iterator forward(block, BytewiseComparator());
    Block::Iterator backward(block, BytewiseComparator());
    ExpectOk(forward.SeekToFirst());
    ExpectOk(backward.SeekToLast());

    ExpectCorruption(forward.Next());
    EXPECT_FALSE(forward.valid());
    ExpectAt(backward, "z", "last");
    ExpectOk(forward.SeekToLast());
    ExpectAt(forward, "z", "last");
    ExpectCorruption(backward.Prev());
    EXPECT_FALSE(backward.valid());
    ExpectAt(forward, "z", "last");
    ExpectOk(backward.SeekToFirst());
    ExpectAt(backward, "a", "first");
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

TEST(BlockIteratorTest, DecodedStorageSurvivesSourceDestructionAndBlockMoves) {
    const std::string value(256, 'v');
    BlockBuilder builder(2);
    builder.Add(AsBytes("a"), AsBytes(value));
    builder.Add(AsBytes("b"), AsBytes("next"));
    const auto raw = Materialize(builder.Finish());
    const auto found = std::ranges::search(raw, AsBytes(value));
    ASSERT_FALSE(found.empty());
    const auto value_offset = static_cast<std::size_t>(found.begin() - raw.begin());

    for (const auto type : {BlockCompression::Snappy, BlockCompression::Zstd}) {
        SCOPED_TRACE(static_cast<int>(type));
        std::optional<Block> owner;
        std::optional<Block::Iterator> iterator;
        const std::byte* expected_value = nullptr;
        {
            std::vector<std::byte> compressed;
            ASSERT_TRUE(TryCompressBlock(raw, type, 1, compressed));
            auto decoded = DecompressBlock(compressed, type);
            ASSERT_TRUE(decoded.has_value()) << decoded.error().ToString();
            expected_value = decoded->data.get() + value_offset;
            auto block = Block::Create(BlockContents::Owned(std::move(*decoded)));
            ASSERT_TRUE(block.has_value()) << block.error().ToString();
            EXPECT_EQ(block->size(), raw.size());
            iterator.emplace(*block, BytewiseComparator());
            ASSERT_TRUE(iterator->SeekToFirst().has_value());
            EXPECT_EQ(iterator->value().data(), expected_value);
            owner.emplace(std::move(*block));
        }

        ExpectAt(*iterator, "a", value);
        EXPECT_EQ(iterator->value().data(), expected_value);
        ExpectOk(iterator->Next());
        ExpectAt(*iterator, "b", "next");
        ExpectOk(iterator->Prev());
        ExpectAt(*iterator, "a", value);
        EXPECT_EQ(iterator->value().data(), expected_value);
    }
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

TEST(BlockIteratorTest, LateRestartFailureClearsPublishedEntryAndPositioningRecovers) {
    const auto first = EntryBytes(0, "a", "1");
    const auto entries = Concat({first, EntryBytes(0, "b", "2"), EntryBytes(0, "c", "3")});
    const Block block =
        MakeBlock(WithRestarts(entries, {0, static_cast<std::uint32_t>(first.size()),
                                         static_cast<std::uint32_t>(entries.size() + 1)}));
    Block::Iterator iterator(block, BytewiseComparator());

    for (int recovery = 0; recovery < 2; ++recovery) {
        ExpectOk(iterator.SeekToFirst());
        ExpectAt(iterator, "a", "1");
        ExpectOk(iterator.Next());
        ExpectAt(iterator, "b", "2");
        const Status result = iterator.Next();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::Corruption);
        EXPECT_EQ(result.error().message(), "bad entry in block");
        EXPECT_FALSE(iterator.valid());
        EXPECT_TRUE(iterator.key().empty());
        EXPECT_TRUE(iterator.value().empty());
    }
}

TEST(BlockIteratorTest, InternalKeyModeRejectsShortTargetsAndEntries) {
    const InternalKeyComparator comparator(BytewiseComparator());
    const Block block = MakeBlock(WithRestarts(EntryBytes(0, "short", ""), {0}));
    Block::Iterator iterator(block, comparator, BlockKeyFormat::Internal);

    ExpectCorruption(iterator.Seek(AsBytes("short")));
    EXPECT_FALSE(iterator.valid());
    ExpectCorruption(iterator.SeekToFirst());
    EXPECT_FALSE(iterator.valid());
}

TEST(BlockIteratorTest, InternalKeySeekRejectsAShortRestartKey) {
    const InternalKeyComparator comparator(BytewiseComparator());
    const InternalKey valid = InternalKey::Create(AsBytes("a"), 1, ValueKind::Value).value();
    const auto first = EntryBytes(0, AsStringView(valid.encoded()), "");
    const auto short_restart = EntryBytes(0, "short", "");
    const Block block = MakeBlock(WithRestarts(Concat({first, short_restart}),
                                               {0, static_cast<std::uint32_t>(first.size())}));
    Block::Iterator iterator(block, comparator, BlockKeyFormat::Internal);
    const InternalKey target = InternalKey::Create(AsBytes("z"), 1, ValueKind::Value).value();

    ExpectCorruption(iterator.Seek(target.encoded()));
}

TEST(BlockIteratorTest, UsesTheBlockComparator) {
    const ReverseComparator reverse;
    BlockBuilder builder(1);
    builder.Add(AsBytes("c"), AsBytes("3"));
    builder.Add(AsBytes("b"), AsBytes("2"));
    builder.Add(AsBytes("a"), AsBytes("1"));
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
            byte = static_cast<std::byte>(small_alphabet ? Alphabet[Below(Alphabet.size())]
                                                         : Below(256));
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
        std::ranges::sort(keys, [&](ByteView left, ByteView right) {
            return comparator.Compare(left, right) < 0;
        });
        const auto duplicates = std::ranges::unique(keys, [&](ByteView left, ByteView right) {
            return comparator.Compare(left, right) == 0;
        });
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

    static void ExpectAtEntry(const Block::Iterator& iterator,
                              const std::vector<ModelEntry>& entries, std::size_t index) {
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
            builder.Add(entry.key, entry.value);
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
            const std::vector<std::byte> target = !entries.empty() && Below(2) == 0
                                                      ? entries[Below(entries.size())].key
                                                      : RandomKey();
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
            SCOPED_TRACE(testing::Message()
                         << "round " << round << " interval " << restart_interval);
            CheckBlock(restart_interval, BytewiseComparator());
            CheckBlock(restart_interval, reverse);
        }
    }
}

}  // namespace
}  // namespace modern_leveldb
