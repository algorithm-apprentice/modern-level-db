#include "format/internal_key.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

class TestComparator final : public Comparator {
public:
    int Compare(ByteView left, ByteView right) const noexcept override {
        return BytewiseComparator().Compare(left, right);
    }

    std::string_view Name() const noexcept override { return "test.Comparator"; }

    void FindShortestSeparator(std::vector<std::byte>& start, ByteView limit) const override {
        BytewiseComparator().FindShortestSeparator(start, limit);
    }

    void FindShortSuccessor(std::vector<std::byte>& key) const override {
        BytewiseComparator().FindShortSuccessor(key);
    }
};

class ReverseComparator final : public Comparator {
public:
    int Compare(ByteView left, ByteView right) const noexcept override {
        return BytewiseComparator().Compare(right, left);
    }

    std::string_view Name() const noexcept override { return "test.ReverseComparator"; }

    void FindShortestSeparator(std::vector<std::byte>&, ByteView) const override {}
    void FindShortSuccessor(std::vector<std::byte>&) const override {}
};

class InvalidSeparatorComparator final : public Comparator {
public:
    int Compare(ByteView left, ByteView right) const noexcept override {
        return BytewiseComparator().Compare(left, right);
    }

    std::string_view Name() const noexcept override { return "test.InvalidSeparatorComparator"; }

    void FindShortestSeparator(std::vector<std::byte>& start, ByteView) const override {
        start.assign(1, std::byte{'z'});
    }

    void FindShortSuccessor(std::vector<std::byte>&) const override {}
};

static_assert(std::is_constructible_v<InternalKeyComparator, TestComparator&>);
static_assert(!std::is_constructible_v<InternalKeyComparator, TestComparator&&>);
static_assert(!std::is_constructible_v<InternalKeyComparator, const TestComparator&&>);
static_assert(!std::is_copy_constructible_v<LookupKey>);
static_assert(!std::is_copy_assignable_v<LookupKey>);
static_assert(std::is_move_constructible_v<LookupKey>);
static_assert(std::is_move_assignable_v<LookupKey>);

std::vector<std::byte> Bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const unsigned int value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

InternalKey MakeKey(ByteView user_key, SequenceNumber sequence, ValueKind kind) {
    auto key = InternalKey::Create(user_key, sequence, kind);
    EXPECT_TRUE(key.has_value());
    return std::move(*key);
}

InternalKey MakeKey(std::string_view user_key, SequenceNumber sequence, ValueKind kind) {
    return MakeKey(AsBytes(user_key), sequence, kind);
}

std::vector<std::byte> Encoded(ByteView user_key, SequenceNumber sequence, ValueKind kind) {
    const InternalKey key = MakeKey(user_key, sequence, kind);
    return {key.encoded().begin(), key.encoded().end()};
}

std::vector<std::byte> Encoded(std::string_view user_key, SequenceNumber sequence, ValueKind kind) {
    return Encoded(AsBytes(user_key), sequence, kind);
}

int Sign(int value) noexcept { return (value > 0) - (value < 0); }

int ReferenceCompare(const Comparator& comparator, ByteView left, ByteView right) {
    const InternalKeyView left_key = ParseInternalKey(left).value();
    const InternalKeyView right_key = ParseInternalKey(right).value();
    const int user_order = comparator.Compare(left_key.user_key, right_key.user_key);
    if (user_order != 0) {
        return user_order;
    }
    const auto trailer = [](const InternalKeyView& key) {
        return PackInternalKeyTrailer(key.sequence, key.kind);
    };
    if (trailer(left_key) > trailer(right_key)) {
        return -1;
    }
    if (trailer(left_key) < trailer(right_key)) {
        return 1;
    }
    return 0;
}

TEST(InternalKeyTest, PersistentConstantsMatchLevelDb) {
    EXPECT_EQ(static_cast<std::uint8_t>(ValueKind::Deletion), 0U);
    EXPECT_EQ(static_cast<std::uint8_t>(ValueKind::Value), 1U);
    EXPECT_EQ(SeekValueKind, ValueKind::Value);
    EXPECT_EQ(InternalKeyTrailerSize, 8U);
    EXPECT_EQ(MaxSequenceNumber, (std::uint64_t{1} << 56U) - 1U);
}

TEST(InternalKeyTest, ValueKindAndTrailerHelpersRoundTrip) {
    EXPECT_TRUE(IsValidValueKind(DecodeValueKind(std::byte{0})));
    EXPECT_TRUE(IsValidValueKind(DecodeValueKind(std::byte{1})));
    EXPECT_FALSE(IsValidValueKind(DecodeValueKind(std::byte{2})));

    std::array encoded_kinds{std::byte{1}, std::byte{0x7f}};
    ByteView remaining = encoded_kinds;
    EXPECT_EQ(ConsumeValueKindTrusted(remaining), ValueKind::Value);
    ASSERT_EQ(remaining.size(), 1U);
    EXPECT_EQ(remaining.front(), std::byte{0x7f});

    constexpr SequenceNumber Sequence = 42;
    const std::uint64_t trailer = PackInternalKeyTrailer(Sequence, ValueKind::Deletion);
    EXPECT_EQ(DecodeInternalKeySequence(trailer), Sequence);
    EXPECT_EQ(DecodeInternalKeyValueKind(trailer), ValueKind::Deletion);

    const InternalKey key = MakeKey("key", Sequence, ValueKind::Deletion);
    EXPECT_EQ(DecodeInternalKeyTrailer(key.encoded()), trailer);
    EXPECT_EQ(DecodeInternalKeyValueKind(key.encoded()), ValueKind::Deletion);
}

TEST(LookupKeyTest, MatchesLevelDbGoldenEncodingAndViews) {
    auto key = LookupKey::Create(AsBytes("foo"), 0x00010203040506ULL);

    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(std::vector<std::byte>(key->memtable_key().begin(), key->memtable_key().end()),
              Bytes({
                  0x0b,
                  0x66,
                  0x6f,
                  0x6f,
                  0x01,
                  0x06,
                  0x05,
                  0x04,
                  0x03,
                  0x02,
                  0x01,
                  0x00,
              }));
    EXPECT_EQ(AsStringView(key->user_key()), "foo");
    EXPECT_EQ(key->internal_key().data(), key->memtable_key().data() + 1);

    const auto parsed = ParseInternalKey(key->internal_key());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->sequence, 0x00010203040506ULL);
    EXPECT_EQ(parsed->kind, ValueKind::Value);
    EXPECT_EQ(AsStringView(parsed->user_key), "foo");
}

TEST(LookupKeyTest, SupportsInlineBoundaryAndLongBinaryKeys) {
    const std::vector<std::byte> inline_key(190, std::byte{0x7f});
    const std::vector<std::byte> heap_key(191, std::byte{0x80});
    const std::array binary_key{std::byte{0x00}, std::byte{0xff}, std::byte{0x10}};

    auto inline_lookup = LookupKey::Create(inline_key, MaxSequenceNumber);
    ASSERT_TRUE(inline_lookup.has_value());
    const std::byte* const inline_address = inline_lookup->memtable_key().data();
    LookupKey moved_inline = std::move(*inline_lookup);
    EXPECT_NE(moved_inline.memtable_key().data(), inline_address);
    EXPECT_EQ(inline_lookup->memtable_key().data(), inline_address);
    EXPECT_TRUE(std::ranges::equal(moved_inline.user_key(), inline_key));
    EXPECT_TRUE(inline_lookup->user_key().empty());

    auto heap_lookup = LookupKey::Create(heap_key, MaxSequenceNumber);
    ASSERT_TRUE(heap_lookup.has_value());
    const std::byte* const heap_address = heap_lookup->memtable_key().data();
    LookupKey moved_heap = std::move(*heap_lookup);
    EXPECT_EQ(moved_heap.memtable_key().data(), heap_address);
    EXPECT_NE(heap_lookup->memtable_key().data(), heap_address);
    EXPECT_TRUE(std::ranges::equal(moved_heap.user_key(), heap_key));
    EXPECT_TRUE(heap_lookup->user_key().empty());

    auto binary_lookup = LookupKey::Create(binary_key, MaxSequenceNumber);
    ASSERT_TRUE(binary_lookup.has_value());
    EXPECT_TRUE(std::ranges::equal(binary_lookup->user_key(), binary_key));

    for (const LookupKey* key : {&moved_inline, &moved_heap, &*binary_lookup}) {
        const auto parsed = ParseInternalKey(key->internal_key());
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->sequence, MaxSequenceNumber);
        EXPECT_EQ(parsed->kind, ValueKind::Value);
    }
}

TEST(LookupKeyTest, MovesAndResetsSourceToCanonicalEmptyKey) {
    const std::vector<std::byte> long_key(512, std::byte{'x'});
    auto source_result = LookupKey::Create(long_key, 99);
    ASSERT_TRUE(source_result.has_value());

    LookupKey destination = std::move(*source_result);

    EXPECT_TRUE(std::ranges::equal(destination.user_key(), long_key));
    EXPECT_TRUE(source_result->user_key().empty());
    const auto moved_from = ParseInternalKey(source_result->internal_key());
    ASSERT_TRUE(moved_from.has_value());
    EXPECT_EQ(moved_from->sequence, 0U);
    EXPECT_EQ(moved_from->kind, ValueKind::Value);

    auto replacement_result = LookupKey::Create(AsBytes("replacement"), 7);
    ASSERT_TRUE(replacement_result.has_value());
    destination = std::move(*replacement_result);
    EXPECT_EQ(AsStringView(destination.user_key()), "replacement");
    EXPECT_TRUE(replacement_result->user_key().empty());
}

TEST(LookupKeyTest, RejectsSequenceOutsideInternalKeyRange) {
    const auto key = LookupKey::Create(AsBytes("key"), MaxSequenceNumber + 1U);

    ASSERT_FALSE(key.has_value());
    EXPECT_EQ(key.error().code(), ErrorCode::InvalidArgument);
}

TEST(InternalKeyTest, MatchesLevelDbGoldenEncodings) {
    struct Vector {
        ByteView user_key;
        SequenceNumber sequence;
        ValueKind kind;
        std::vector<std::byte> expected;
    };

    const std::array binary_user_key{std::byte{0x00}, std::byte{0xff}, std::byte{0x10}};
    // Literal expectations separate the layout oracle from encode/decode agreement:
    // "foo" is 66 6f 6f, then (sequence << 8 | kind) in little-endian bytes.
    // Sequence 256 with Deletion therefore starts its trailer 00 00 01.
    const std::vector<Vector> vectors{
        {AsBytes(""), 0, ValueKind::Deletion,
         Bytes({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})},
        {AsBytes(""), MaxSequenceNumber, ValueKind::Value,
         Bytes({0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})},
        {AsBytes("foo"), 1, ValueKind::Value,
         Bytes({0x66, 0x6f, 0x6f, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})},
        {AsBytes("foo"), 256, ValueKind::Deletion,
         Bytes({0x66, 0x6f, 0x6f, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00})},
        {binary_user_key, 0x00010203040506ULL, ValueKind::Value,
         Bytes({0x00, 0xff, 0x10, 0x01, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00})},
    };

    for (const auto& vector : vectors) {
        SCOPED_TRACE(vector.sequence);
        auto key = InternalKey::Create(vector.user_key, vector.sequence, vector.kind);
        ASSERT_TRUE(key.has_value());
        EXPECT_EQ(std::vector<std::byte>(key->encoded().begin(), key->encoded().end()),
                  vector.expected);

        const auto parsed = ParseInternalKey(vector.expected);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_TRUE(std::ranges::equal(parsed->user_key, vector.user_key));
        EXPECT_EQ(parsed->sequence, vector.sequence);
        EXPECT_EQ(parsed->kind, vector.kind);

        const auto decoded = InternalKey::Decode(vector.expected);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_TRUE(std::ranges::equal(decoded->encoded(), key->encoded()));
        EXPECT_TRUE(std::ranges::equal(decoded->user_key(), vector.user_key));
        EXPECT_EQ(decoded->sequence(), vector.sequence);
        EXPECT_EQ(decoded->kind(), vector.kind);
    }
}

TEST(InternalKeyTest, RoundTripsSequenceBoundaries) {
    constexpr std::array<SequenceNumber, 13> Sequences{
        0,
        1,
        (std::uint64_t{1} << 8U) - 1U,
        std::uint64_t{1} << 8U,
        (std::uint64_t{1} << 16U) - 1U,
        std::uint64_t{1} << 16U,
        (std::uint64_t{1} << 32U) - 1U,
        std::uint64_t{1} << 32U,
        (std::uint64_t{1} << 48U) - 1U,
        std::uint64_t{1} << 48U,
        MaxSequenceNumber - 1U,
        MaxSequenceNumber,
        42,
    };

    for (const SequenceNumber sequence : Sequences) {
        for (const ValueKind kind : {ValueKind::Deletion, ValueKind::Value}) {
            SCOPED_TRACE(sequence);
            const InternalKey key = MakeKey("key", sequence, kind);
            const auto parsed = ParseInternalKey(key.encoded());
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(AsStringView(parsed->user_key), "key");
            EXPECT_EQ(parsed->sequence, sequence);
            EXPECT_EQ(parsed->kind, kind);
        }
    }
}

TEST(InternalKeyTest, ParsedUserKeyBorrowsEncodedStorage) {
    const InternalKey key = MakeKey("borrowed", 7, ValueKind::Value);

    const auto parsed = ParseInternalKey(key.encoded());

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->user_key.data(), key.encoded().data());
}

TEST(InternalKeyTest, RejectsInvalidConstructionInputs) {
    const auto large_sequence =
        InternalKey::Create(AsBytes("key"), MaxSequenceNumber + 1U, ValueKind::Value);
    ASSERT_FALSE(large_sequence.has_value());
    EXPECT_EQ(large_sequence.error().code(), ErrorCode::InvalidArgument);

    const auto invalid_kind = InternalKey::Create(AsBytes("key"), 1, static_cast<ValueKind>(2));
    ASSERT_FALSE(invalid_kind.has_value());
    EXPECT_EQ(invalid_kind.error().code(), ErrorCode::InvalidArgument);
}

TEST(InternalKeyTest, RejectsTruncatedAndUnknownPersistentKinds) {
    for (std::size_t size = 0; size < InternalKeyTrailerSize; ++size) {
        SCOPED_TRACE(size);
        const std::vector<std::byte> truncated(size, std::byte{0});
        const auto parsed = ParseInternalKey(truncated);
        ASSERT_FALSE(parsed.has_value());
        EXPECT_EQ(parsed.error().code(), ErrorCode::Corruption);

        const auto decoded = InternalKey::Decode(truncated);
        ASSERT_FALSE(decoded.has_value());
        EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
    }

    std::vector<std::byte> invalid_kind(InternalKeyTrailerSize, std::byte{0});
    invalid_kind[0] = std::byte{2};
    const auto parsed = ParseInternalKey(invalid_kind);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code(), ErrorCode::Corruption);
}

TEST(InternalKeyTest, OwningCopiesHaveIndependentStorage) {
    InternalKey original = MakeKey("copy", 99, ValueKind::Value);
    const InternalKey copy = original;
    const std::vector<std::byte> source(copy.encoded().begin(), copy.encoded().end());

    EXPECT_TRUE(std::ranges::equal(copy.encoded(), original.encoded()));
    EXPECT_NE(copy.encoded().data(), original.encoded().data());

    auto replacement = InternalKey::Create(AsBytes("replacement"), 1, ValueKind::Deletion);
    ASSERT_TRUE(replacement.has_value());
    original = std::move(*replacement);
    EXPECT_TRUE(std::ranges::equal(copy.encoded(), source));
}

TEST(InternalKeyComparatorTest, UsesLevelDbName) {
    EXPECT_EQ(InternalKeyComparator(BytewiseComparator()).Name(), "leveldb.InternalKeyComparator");
}

TEST(InternalKeyComparatorTest, ExposesItsUserComparator) {
    const InternalKeyComparator comparator(BytewiseComparator());

    EXPECT_EQ(&comparator.user_comparator(), &BytewiseComparator());
}

TEST(InternalKeyComparatorTest, OrdersUserKeyAscendingAndTrailerDescending) {
    InternalKeyComparator comparator(BytewiseComparator());
    const InternalKey a_new = MakeKey("a", 100, ValueKind::Value);
    const InternalKey a_old = MakeKey("a", 99, ValueKind::Value);
    const InternalKey a_deleted = MakeKey("a", 100, ValueKind::Deletion);
    const InternalKey b = MakeKey("b", 1, ValueKind::Value);

    EXPECT_LT(comparator.Compare(a_new, a_old), 0);
    EXPECT_LT(comparator.Compare(a_new, a_deleted), 0);
    EXPECT_LT(comparator.Compare(a_old, b), 0);
    EXPECT_GT(comparator.Compare(b, a_new), 0);
    EXPECT_EQ(comparator.Compare(a_new, a_new), 0);
}

TEST(InternalKeyComparatorTest, MatchesDecodedReferenceForValidKeys) {
    std::mt19937_64 random(20260924);
    std::uniform_int_distribution<std::size_t> length(0, 64);
    std::uniform_int_distribution<unsigned int> byte(0, 255);
    std::uniform_int_distribution<SequenceNumber> sequence(0, MaxSequenceNumber);
    std::uniform_int_distribution<unsigned int> kind(0, 1);
    const ReverseComparator reverse;

    for (const Comparator* user_comparator :
         {&BytewiseComparator(), static_cast<const Comparator*>(&reverse)}) {
        const InternalKeyComparator comparator(*user_comparator);
        for (int iteration = 0; iteration < 5000; ++iteration) {
            std::vector<std::byte> left_user(length(random));
            std::vector<std::byte> right_user(length(random));
            for (std::byte& value : left_user) {
                value = static_cast<std::byte>(byte(random));
            }
            for (std::byte& value : right_user) {
                value = static_cast<std::byte>(byte(random));
            }
            const std::vector<std::byte> left =
                Encoded(left_user, sequence(random), static_cast<ValueKind>(kind(random)));
            const std::vector<std::byte> right =
                Encoded(right_user, sequence(random), static_cast<ValueKind>(kind(random)));

            EXPECT_EQ(Sign(comparator.Compare(left, right)),
                      Sign(ReferenceCompare(*user_comparator, left, right)));
        }
    }
}

#ifndef NDEBUG
TEST(InternalKeyComparatorDeathTest, ComparisonRejectsShortOperands) {
    const InternalKeyComparator comparator(BytewiseComparator());
    const std::vector<std::byte> short_key(InternalKeyTrailerSize - 1);
    const std::vector<std::byte> valid = Encoded("valid", 1, ValueKind::Value);

    EXPECT_DEATH(static_cast<void>(comparator.Compare(short_key, valid)), "");
    EXPECT_DEATH(static_cast<void>(comparator.Compare(valid, short_key)), "");
}
#endif

TEST(InternalKeyComparatorTest, MatchesLevelDbShortestSeparatorCases) {
    InternalKeyComparator comparator(BytewiseComparator());

    for (const auto& limit :
         {Encoded("foo", 99, ValueKind::Value), Encoded("foo", 101, ValueKind::Value),
          Encoded("foo", 100, ValueKind::Value), Encoded("foo", 100, ValueKind::Deletion),
          Encoded("bar", 99, ValueKind::Value), Encoded("foobar", 200, ValueKind::Value)}) {
        auto unchanged = Encoded("foo", 100, ValueKind::Value);
        comparator.FindShortestSeparator(unchanged, limit);
        EXPECT_EQ(unchanged, Encoded("foo", 100, ValueKind::Value));
    }

    auto shortened = Encoded("foo", 100, ValueKind::Value);
    const auto limit = Encoded("hello", 200, ValueKind::Value);
    comparator.FindShortestSeparator(shortened, limit);
    EXPECT_EQ(shortened, Encoded("g", MaxSequenceNumber, SeekValueKind));

    auto reverse_prefix = Encoded("foobar", 100, ValueKind::Value);
    const auto reverse_limit = Encoded("foo", 200, ValueKind::Value);
    comparator.FindShortestSeparator(reverse_prefix, reverse_limit);
    EXPECT_EQ(reverse_prefix, Encoded("foobar", 100, ValueKind::Value));
}

TEST(InternalKeyComparatorTest, RejectsASeparatorAtOrPastTheLimit) {
    const InvalidSeparatorComparator invalid;
    InternalKeyComparator comparator(invalid);
    const auto original = Encoded("aa", 100, ValueKind::Value);
    auto start = original;
    const auto limit = Encoded("c", 200, ValueKind::Value);

    comparator.FindShortestSeparator(start, limit);

    EXPECT_EQ(start, original);
}

TEST(InternalKeyComparatorTest, MatchesLevelDbShortSuccessorCases) {
    InternalKeyComparator comparator(BytewiseComparator());

    auto key = Encoded("foo", 100, ValueKind::Value);
    comparator.FindShortSuccessor(key);
    EXPECT_EQ(key, Encoded("g", MaxSequenceNumber, SeekValueKind));

    auto all_ff =
        Encoded(ByteView(std::array{std::byte{0xff}, std::byte{0xff}}), 100, ValueKind::Value);
    const auto original = all_ff;
    comparator.FindShortSuccessor(all_ff);
    EXPECT_EQ(all_ff, original);
}

TEST(InternalKeyComparatorTest, LeavesMalformedKeysUnchangedWhenShortening) {
    InternalKeyComparator comparator(BytewiseComparator());
    std::vector<std::byte> malformed{std::byte{'x'}};
    const InternalKey limit = MakeKey("z", 1, ValueKind::Value);

    comparator.FindShortestSeparator(malformed, limit.encoded());
    ASSERT_EQ(malformed.size(), 1U);
    EXPECT_EQ(malformed.front(), std::byte{'x'});
    comparator.FindShortSuccessor(malformed);
    ASSERT_EQ(malformed.size(), 1U);
    EXPECT_EQ(malformed.front(), std::byte{'x'});
}

}  // namespace
}  // namespace modern_leveldb
