#include "format/internal_key.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
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

struct ShorteningCall {
    char operation = '\0';
    std::array<std::byte, 8> left{};
    std::array<std::byte, 8> right{};
    std::size_t left_size = 0;
    std::size_t right_size = 0;
};

class RecordingShortener final : public Comparator {
public:
    explicit RecordingShortener(std::string_view candidate, bool throws = false)
        : candidate_(candidate), throws_(throws) {}

    int Compare(ByteView left, ByteView right) const noexcept override {
        Record('C', left, right);
        return 17 * BytewiseComparator().Compare(left, right);
    }

    std::string_view Name() const noexcept override { return "test.RecordingShortener"; }

    void FindShortestSeparator(std::vector<std::byte>& start, ByteView limit) const override {
        Record('S', start, limit);
        Shorten(start);
    }

    void FindShortSuccessor(std::vector<std::byte>& key) const override {
        Record('U', key, {});
        Shorten(key);
    }

    mutable std::array<ShorteningCall, 4> calls{};
    mutable std::size_t call_count = 0;

private:
    void Record(char operation, ByteView left, ByteView right) const noexcept {
        const std::size_t index = call_count++;
        if (index < calls.size()) {
            auto& call = calls[index];
            call.operation = operation;
            call.left_size = left.size();
            call.right_size = right.size();
            std::copy_n(left.begin(), std::min(left.size(), call.left.size()), call.left.begin());
            std::copy_n(right.begin(), std::min(right.size(), call.right.size()),
                        call.right.begin());
        }
    }

    void Shorten(std::vector<std::byte>& key) const {
        const ByteView candidate = AsBytes(candidate_);
        key.assign(candidate.begin(), candidate.end());
        if (throws_) {
            throw std::runtime_error("shortener failed");
        }
    }

    std::string_view candidate_;
    bool throws_;
};

void ExpectShorteningCall(const ShorteningCall& call, char operation, std::string_view left,
                          std::string_view right = {}) {
    EXPECT_EQ(call.operation, operation);
    ASSERT_LE(call.left_size, call.left.size());
    ASSERT_LE(call.right_size, call.right.size());
    EXPECT_EQ(AsStringView(ByteView(call.left).first(call.left_size)), left);
    EXPECT_EQ(AsStringView(ByteView(call.right).first(call.right_size)), right);
}

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

void ExpectLookupRepresentation(const LookupKey& key, ByteView encoded, std::size_t prefix_size) {
    EXPECT_TRUE(std::ranges::equal(key.memtable_key(), encoded));
    EXPECT_EQ(key.internal_key().data(), key.memtable_key().data() + prefix_size);
    EXPECT_TRUE(std::ranges::equal(key.internal_key(), encoded.subspan(prefix_size)));
    EXPECT_EQ(key.user_key().data(), key.internal_key().data());
    EXPECT_TRUE(std::ranges::equal(
        key.user_key(), encoded.subspan(prefix_size).first(encoded.size() - prefix_size - 8)));
}

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

TEST(LookupKeyTest, LiteralPrefixAndTrailerBoundariesPreserveGuardedCallerBytes) {
    struct LiteralLayout {
        std::size_t user_size;
        std::vector<std::byte> prefix;
    };
    const std::array layouts{
        LiteralLayout{0, Bytes({0x08})},         LiteralLayout{118, Bytes({0x7e})},
        LiteralLayout{119, Bytes({0x7f})},       LiteralLayout{120, Bytes({0x80, 0x01})},
        LiteralLayout{190, Bytes({0xc6, 0x01})}, LiteralLayout{191, Bytes({0xc7, 0x01})},
        LiteralLayout{512, Bytes({0x88, 0x04})},
    };
    const std::array pattern{std::byte{0x00}, std::byte{0xff}, std::byte{0x10}, std::byte{0x80}};
    for (const auto& layout : layouts) {
        for (const SequenceNumber sequence : {SequenceNumber{0}, MaxSequenceNumber}) {
            SCOPED_TRACE(layout.user_size);
            SCOPED_TRACE(sequence);
            std::vector<std::byte> guarded(layout.user_size + 2);
            guarded.front() = std::byte{0xa5};
            guarded.back() = std::byte{0x5a};
            for (std::size_t index = 0; index < layout.user_size; ++index) {
                guarded[index + 1] = pattern[index % pattern.size()];
            }
            const std::vector<std::byte> before = guarded;
            const ByteView user = ByteView(guarded).subspan(1, layout.user_size);
            const std::vector<std::byte> trailer =
                sequence == 0 ? Bytes({0x01, 0, 0, 0, 0, 0, 0, 0})
                              : Bytes({0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
            std::vector<std::byte> expected = layout.prefix;
            expected.insert(expected.end(), user.begin(), user.end());
            expected.insert(expected.end(), trailer.begin(), trailer.end());

            const auto checked = LookupKey::Create(user, sequence);
            ASSERT_TRUE(checked.has_value());
            const LookupKey trusted = LookupKey::CreateTrusted(user, sequence);

            ExpectLookupRepresentation(*checked, expected, layout.prefix.size());
            ExpectLookupRepresentation(trusted, expected, layout.prefix.size());
            EXPECT_NE(checked->user_key().data(), user.data());
            EXPECT_NE(trusted.user_key().data(), user.data());
            EXPECT_EQ(guarded, before);
        }
    }
}

TEST(LookupKeyTest, MoveMatrixKeepsCompleteOwnersAndExactCanonicalSources) {
    constexpr std::array<std::size_t, 4> Sizes{0, 190, 191, 512};
    const std::vector<std::byte> canonical = Bytes({0x08, 0x01, 0, 0, 0, 0, 0, 0, 0});
    for (const std::size_t source_size : Sizes) {
        SCOPED_TRACE(source_size);
        const std::vector<std::byte> user(source_size, std::byte{0xff});
        auto constructed_source = LookupKey::Create(user, MaxSequenceNumber);
        ASSERT_TRUE(constructed_source.has_value());
        const std::vector<std::byte> expected(constructed_source->memtable_key().begin(),
                                              constructed_source->memtable_key().end());
        const std::size_t prefix_size = expected.size() - source_size - 8;
        const std::byte* const construction_address = constructed_source->memtable_key().data();

        LookupKey constructed = std::move(*constructed_source);

        ExpectLookupRepresentation(constructed, expected, prefix_size);
        ExpectLookupRepresentation(*constructed_source, canonical, 1);
        EXPECT_EQ(constructed.memtable_key().data() == construction_address, source_size >= 191);
        for (const std::size_t destination_size : Sizes) {
            SCOPED_TRACE(destination_size);
            auto source = LookupKey::Create(user, MaxSequenceNumber);
            ASSERT_TRUE(source.has_value());
            const std::vector<std::byte> old_destination(destination_size, std::byte{0x00});
            auto destination = LookupKey::Create(old_destination, 7);
            ASSERT_TRUE(destination.has_value());
            const std::byte* const source_address = source->memtable_key().data();

            *destination = std::move(*source);

            ExpectLookupRepresentation(*destination, expected, prefix_size);
            ExpectLookupRepresentation(*source, canonical, 1);
            EXPECT_EQ(destination->memtable_key().data() == source_address, source_size >= 191);
            const std::byte* const destination_address = destination->memtable_key().data();
            LookupKey* const self = &*destination;

            *destination = std::move(*self);

            ExpectLookupRepresentation(*destination, expected, prefix_size);
            EXPECT_EQ(destination->memtable_key().data(), destination_address);
        }
    }
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

TEST(InternalKeyTest, ConstructionAdmissionKeepsExactSequenceBeforeKindPriority) {
    struct InvalidInput {
        SequenceNumber sequence;
        ValueKind kind;
        std::string_view message;
    };
    const std::array invalid{
        InvalidInput{MaxSequenceNumber + 1, static_cast<ValueKind>(2),
                     "internal key sequence exceeds 56 bits"},
        InvalidInput{MaxSequenceNumber + 1, ValueKind::Value,
                     "internal key sequence exceeds 56 bits"},
        InvalidInput{0, static_cast<ValueKind>(2), "internal key value kind is unsupported"},
        InvalidInput{MaxSequenceNumber, static_cast<ValueKind>(0xff),
                     "internal key value kind is unsupported"},
    };
    for (const auto& test : invalid) {
        SCOPED_TRACE(test.message);
        const auto key = InternalKey::Create(AsBytes("s"), test.sequence, test.kind);
        ASSERT_FALSE(key.has_value());
        EXPECT_EQ(key.error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(key.error().message(), test.message);
    }
    struct ValidInput {
        SequenceNumber sequence;
        ValueKind kind;
        std::vector<std::byte> encoded;
    };
    const std::array valid{
        ValidInput{0, ValueKind::Deletion, Bytes({0x73, 0, 0, 0, 0, 0, 0, 0, 0})},
        ValidInput{0, ValueKind::Value, Bytes({0x73, 0x01, 0, 0, 0, 0, 0, 0, 0})},
        ValidInput{MaxSequenceNumber, ValueKind::Deletion,
                   Bytes({0x73, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})},
        ValidInput{MaxSequenceNumber, ValueKind::Value,
                   Bytes({0x73, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})},
    };
    for (const auto& test : valid) {
        SCOPED_TRACE(test.sequence);
        const auto key = InternalKey::Create(AsBytes("s"), test.sequence, test.kind);
        ASSERT_TRUE(key.has_value());
        EXPECT_TRUE(std::ranges::equal(key->encoded(), test.encoded));
    }
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

TEST(InternalKeyComparatorTest, ShorteningPoliciesKeepCallbacksAndDelayedPublication) {
    struct CandidateCase {
        std::string_view candidate;
        std::string_view limit;
        SequenceNumber limit_sequence;
        bool separator_changes;
        bool successor_changes;
        std::size_t separator_compares;
        std::size_t successor_compares;
    };
    const std::array cases{
        CandidateCase{"abcd", "z", 100, false, false, 0, 0},
        CandidateCase{"abcdef", "z", 100, false, false, 0, 0},
        CandidateCase{"ab", "z", 100, false, false, 1, 1},
        CandidateCase{"bc", "bb", 100, false, true, 2, 1},
        CandidateCase{"bb", "bb", MaxSequenceNumber, false, true, 2, 1},
        CandidateCase{"bb", "bb", 100, true, true, 2, 1},
        CandidateCase{"b", "c", 100, true, true, 2, 1},
    };
    const auto original = Encoded("abcd", 100, ValueKind::Value);
    const auto trailer = Bytes({0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
    for (const auto& test : cases) {
        SCOPED_TRACE(test.candidate);
        SCOPED_TRACE(test.limit_sequence);
        const auto limit = Encoded(test.limit, test.limit_sequence, ValueKind::Value);
        const ByteView candidate = AsBytes(test.candidate);
        std::vector<std::byte> expected(candidate.begin(), candidate.end());
        expected.insert(expected.end(), trailer.begin(), trailer.end());

        const RecordingShortener separator_user(test.candidate);
        const InternalKeyComparator separator(separator_user);
        auto start = original;
        separator.FindShortestSeparator(start, limit);
        EXPECT_EQ(start, test.separator_changes ? expected : original);
        ASSERT_EQ(separator_user.call_count, 1 + test.separator_compares);
        ExpectShorteningCall(separator_user.calls[0], 'S', "abcd", test.limit);
        if (test.separator_compares != 0) {
            ExpectShorteningCall(separator_user.calls[1], 'C', "abcd", test.candidate);
        }
        if (test.separator_compares == 2) {
            ExpectShorteningCall(separator_user.calls[2], 'C', test.candidate, test.limit);
        }

        const RecordingShortener successor_user(test.candidate);
        const InternalKeyComparator successor(successor_user);
        auto key = original;
        successor.FindShortSuccessor(key);
        EXPECT_EQ(key, test.successor_changes ? expected : original);
        ASSERT_EQ(successor_user.call_count, 1 + test.successor_compares);
        ExpectShorteningCall(successor_user.calls[0], 'U', "abcd");
        if (test.successor_compares != 0) {
            ExpectShorteningCall(successor_user.calls[1], 'C', "abcd", test.candidate);
        }
    }

    const RecordingShortener user("");
    const InternalKeyComparator comparator(user);
    const auto a = Encoded("a", 9, ValueKind::Value);
    const auto b = Encoded("b", 1, ValueKind::Value);
    const auto older_a = Encoded("a", 8, ValueKind::Value);
    EXPECT_EQ(comparator.Compare(a, b), -17);
    EXPECT_EQ(comparator.Compare(b, a), 17);
    EXPECT_EQ(comparator.Compare(a, older_a), -1);
    ASSERT_EQ(user.call_count, 3U);
    ExpectShorteningCall(user.calls[0], 'C', "a", "b");
    ExpectShorteningCall(user.calls[1], 'C', "b", "a");
    ExpectShorteningCall(user.calls[2], 'C', "a", "a");
}

TEST(InternalKeyComparatorTest, MalformedAndThrowingShortenersKeepOriginalOwners) {
    const auto original = Encoded("abcd", 100, ValueKind::Value);
    const auto limit = Encoded("z", 100, ValueKind::Value);
    const std::array malformed{
        std::vector<std::byte>{},
        Bytes({0xff}),
        Bytes({0x02, 0, 0, 0, 0, 0, 0, 0}),
    };
    for (const auto& bad : malformed) {
        const RecordingShortener user("b");
        const InternalKeyComparator comparator(user);
        auto start = bad;
        comparator.FindShortestSeparator(start, limit);
        EXPECT_EQ(start, bad);
        comparator.FindShortSuccessor(start);
        EXPECT_EQ(start, bad);
        start = original;
        comparator.FindShortestSeparator(start, bad);
        EXPECT_EQ(start, original);
        EXPECT_EQ(user.call_count, 0U);
    }

    const RecordingShortener separator_user("b", true);
    const InternalKeyComparator separator(separator_user);
    auto start = original;
    EXPECT_THROW(separator.FindShortestSeparator(start, limit), std::runtime_error);
    EXPECT_EQ(start, original);
    ASSERT_EQ(separator_user.call_count, 1U);
    ExpectShorteningCall(separator_user.calls[0], 'S', "abcd", "z");

    const RecordingShortener successor_user("b", true);
    const InternalKeyComparator successor(successor_user);
    auto key = original;
    EXPECT_THROW(successor.FindShortSuccessor(key), std::runtime_error);
    EXPECT_EQ(key, original);
    ASSERT_EQ(successor_user.call_count, 1U);
    ExpectShorteningCall(successor_user.calls[0], 'U', "abcd");
}

}  // namespace
}  // namespace modern_leveldb
