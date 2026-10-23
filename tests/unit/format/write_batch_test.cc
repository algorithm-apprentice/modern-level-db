#include "format/write_batch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

static_assert(std::is_copy_constructible_v<EncodedWriteBatch>);
static_assert(std::is_copy_assignable_v<EncodedWriteBatch>);
static_assert(std::is_move_constructible_v<EncodedWriteBatch>);
static_assert(std::is_move_assignable_v<EncodedWriteBatch>);
static_assert(noexcept(std::declval<EncodedWriteBatch&>().Clear()));
static_assert(!std::is_copy_constructible_v<WriteBatchReader>);
static_assert(!std::is_copy_assignable_v<WriteBatchReader>);
static_assert(std::is_move_constructible_v<WriteBatchReader>);
static_assert(std::is_move_assignable_v<WriteBatchReader>);

std::vector<std::byte> Bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const unsigned int value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

std::vector<std::byte> Materialize(ByteView value) { return {value.begin(), value.end()}; }

std::vector<std::byte> Pattern(std::size_t size, unsigned int seed) {
    std::vector<std::byte> result;
    result.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(static_cast<std::byte>((index * 29U + seed) & 0xffU));
    }
    return result;
}

struct LiteralBatchRecord {
    ValueKind kind;
    ByteView key;
    ByteView value;
};

void ExpectLiteralLengthAndBytes(ByteView encoded, std::size_t& offset, ByteView expected) {
    if (expected.size() == 127U) {
        ASSERT_LT(offset, encoded.size());
        EXPECT_EQ(encoded[offset++], std::byte{0x7f});
    } else {
        ASSERT_EQ(expected.size(), 128U);
        ASSERT_LE(offset + 2U, encoded.size());
        EXPECT_EQ(encoded[offset++], std::byte{0x80});
        EXPECT_EQ(encoded[offset++], std::byte{0x01});
    }
    ASSERT_LE(offset + expected.size(), encoded.size());
    EXPECT_TRUE(std::ranges::equal(encoded.subspan(offset, expected.size()), expected));
    offset += expected.size();
}

void ExpectLiteralBatch(ByteView encoded, std::initializer_list<LiteralBatchRecord> records) {
    ASSERT_GE(encoded.size(), WriteBatchHeaderSize);
    for (std::size_t index = 0; index < sizeof(SequenceNumber); ++index) {
        EXPECT_EQ(encoded[index], std::byte{0});
    }
    EXPECT_EQ(encoded[8], static_cast<std::byte>(records.size()));
    EXPECT_EQ(encoded[9], std::byte{0});
    EXPECT_EQ(encoded[10], std::byte{0});
    EXPECT_EQ(encoded[11], std::byte{0});

    std::size_t offset = WriteBatchHeaderSize;
    for (const LiteralBatchRecord& record : records) {
        ASSERT_LT(offset, encoded.size());
        EXPECT_EQ(encoded[offset++], static_cast<std::byte>(record.kind));
        ExpectLiteralLengthAndBytes(encoded, offset, record.key);
        if (record.kind == ValueKind::Value) {
            ExpectLiteralLengthAndBytes(encoded, offset, record.value);
        }
    }
    EXPECT_EQ(offset, encoded.size());
}

void MoveAssign(EncodedWriteBatch& destination, EncodedWriteBatch&& source) {
    destination = std::move(source);
}

std::vector<std::byte> EncodedBatch(SequenceNumber sequence, std::uint32_t count,
                                    ByteView records = {}) {
    std::vector<std::byte> encoded;
    AppendFixed64(encoded, sequence);
    AppendFixed32(encoded, count);
    encoded.insert(encoded.end(), records.begin(), records.end());
    return encoded;
}

void ExpectCorruption(ByteView encoded, std::string_view message) {
    const auto reader = WriteBatchReader::Open(encoded);
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error().code(), ErrorCode::Corruption);
    EXPECT_EQ(reader.error().message(), message);
}

TEST(WriteBatchTest, PersistentConstantsAndDefaultHeaderMatchLevelDb) {
    EXPECT_EQ(WriteBatchHeaderSize, 12U);

    const EncodedWriteBatch batch;

    EXPECT_EQ(batch.sequence(), 0U);
    EXPECT_EQ(batch.count(), 0U);
    EXPECT_EQ(Materialize(batch.encoded()), std::vector<std::byte>(WriteBatchHeaderSize));
}

TEST(WriteBatchTest, MatchesLevelDbGoldenPutAndDeleteEncoding) {
    EncodedWriteBatch batch;
    batch.SetSequence(0x00010203040506ULL);
    batch.Put(AsBytes("foo"), AsBytes("bar"));
    batch.Delete(AsBytes("box"));

    EXPECT_EQ(Materialize(batch.encoded()),
              Bytes({
                  0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
                  0x03, 0x66, 0x6f, 0x6f, 0x03, 0x62, 0x61, 0x72, 0x00, 0x03, 0x62, 0x6f, 0x78,
              }));
}

TEST(WriteBatchTest, EncodesEmptyBinaryAndMultiByteLengths) {
    const std::array binary_key{std::byte{0x00}, std::byte{0xff}};
    const std::vector<std::byte> long_key(128, std::byte{'k'});
    EncodedWriteBatch batch;

    batch.Put({}, {});
    batch.Delete(binary_key);
    batch.Put(long_key, AsBytes("v"));

    const ByteView encoded = batch.encoded();
    ASSERT_GE(encoded.size(), WriteBatchHeaderSize + 3U + 4U + 4U);
    EXPECT_EQ(encoded[WriteBatchHeaderSize], std::byte{0x01});
    EXPECT_EQ(encoded[WriteBatchHeaderSize + 1U], std::byte{0x00});
    EXPECT_EQ(encoded[WriteBatchHeaderSize + 2U], std::byte{0x00});
    EXPECT_EQ(encoded[WriteBatchHeaderSize + 3U], std::byte{0x00});
    EXPECT_EQ(encoded[WriteBatchHeaderSize + 4U], std::byte{0x02});
    EXPECT_EQ(encoded[WriteBatchHeaderSize + 5U], std::byte{0x00});
    EXPECT_EQ(encoded[WriteBatchHeaderSize + 6U], std::byte{0xff});
    EXPECT_EQ(encoded[WriteBatchHeaderSize + 7U], std::byte{0x01});
    EXPECT_EQ(encoded[WriteBatchHeaderSize + 8U], std::byte{0x80});
    EXPECT_EQ(encoded[WriteBatchHeaderSize + 9U], std::byte{0x01});
}

TEST(WriteBatchTest, LiteralLengthBoundariesCommitCompleteRecordsAndCounts) {
    const std::vector<std::byte> key127 = Pattern(127, 3);
    const std::vector<std::byte> key128 = Pattern(128, 5);
    const std::vector<std::byte> value127 = Pattern(127, 7);
    const std::vector<std::byte> value128 = Pattern(128, 11);
    EncodedWriteBatch batch;

    batch.Put(key127, value128);
    ExpectLiteralBatch(batch.encoded(), {{ValueKind::Value, key127, value128}});

    batch.Delete(key128);
    ExpectLiteralBatch(batch.encoded(),
                       {{ValueKind::Value, key127, value128}, {ValueKind::Deletion, key128, {}}});

    batch.Put(key128, value127);
    ExpectLiteralBatch(batch.encoded(), {{ValueKind::Value, key127, value128},
                                         {ValueKind::Deletion, key128, {}},
                                         {ValueKind::Value, key128, value127}});
}

TEST(WriteBatchTest, AppendingAcrossLengthWidthsPreservesHeaderAndExistingRecords) {
    for (const std::size_t length : {0U, 1U, 127U, 128U, 16'383U, 16'384U}) {
        SCOPED_TRACE(length);
        const auto key = Pattern(length, 23);
        const auto value = Pattern(length, 31);
        EncodedWriteBatch batch;
        batch.SetSequence(0x00010203040506ULL);
        batch.Put(AsBytes("prefix"), AsBytes("stable"));
        const auto before = Materialize(batch.encoded());

        batch.Put(key, value);
        batch.Delete(key);

        EXPECT_EQ(batch.sequence(), 0x00010203040506ULL);
        EXPECT_EQ(batch.count(), 3U);
        EXPECT_TRUE(std::ranges::equal(ByteView(before).first<8>(), batch.encoded().first<8>()));
        EXPECT_TRUE(std::ranges::equal(
            ByteView(before).subspan(WriteBatchHeaderSize),
            batch.encoded().subspan(WriteBatchHeaderSize, before.size() - WriteBatchHeaderSize)));
        auto reader = WriteBatchReader::OpenTrusted(batch);
        ASSERT_TRUE(reader.Next().has_value());
        const auto put = reader.Next();
        const auto deleted = reader.Next();
        ASSERT_TRUE(put.has_value());
        ASSERT_TRUE(deleted.has_value());
        EXPECT_TRUE(std::ranges::equal(put->key, key));
        EXPECT_TRUE(std::ranges::equal(put->value, value));
        EXPECT_EQ(deleted->kind, ValueKind::Deletion);
        EXPECT_TRUE(std::ranges::equal(deleted->key, key));
        EXPECT_TRUE(deleted->value.empty());
        EXPECT_FALSE(reader.Next().has_value());
    }
}

TEST(WriteBatchReaderTest, IteratesBorrowedEntriesWithAssignedSequences) {
    EncodedWriteBatch batch;
    batch.SetSequence(100);
    batch.Put(AsBytes("alpha"), AsBytes("one"));
    batch.Delete(AsBytes("beta"));
    batch.Put(AsBytes("gamma"), {});

    auto opened = WriteBatchReader::Open(batch.encoded());
    ASSERT_TRUE(opened.has_value());
    WriteBatchReader reader = std::move(*opened);
    EXPECT_EQ(reader.sequence(), 100U);
    EXPECT_EQ(reader.count(), 3U);

    const std::optional<WriteBatchEntry> first = reader.Next();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->sequence, 100U);
    EXPECT_EQ(first->kind, ValueKind::Value);
    EXPECT_EQ(AsStringView(first->key), "alpha");
    EXPECT_EQ(AsStringView(first->value), "one");
    EXPECT_GE(first->key.data(), batch.encoded().data());
    EXPECT_LT(first->key.data(), batch.encoded().data() + batch.encoded().size());

    const std::optional<WriteBatchEntry> second = reader.Next();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->sequence, 101U);
    EXPECT_EQ(second->kind, ValueKind::Deletion);
    EXPECT_EQ(AsStringView(second->key), "beta");
    EXPECT_TRUE(second->value.empty());

    const std::optional<WriteBatchEntry> third = reader.Next();
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(third->sequence, 102U);
    EXPECT_EQ(third->kind, ValueKind::Value);
    EXPECT_EQ(AsStringView(third->key), "gamma");
    EXPECT_TRUE(third->value.empty());

    EXPECT_FALSE(reader.Next().has_value());
    EXPECT_FALSE(reader.Next().has_value());
}

TEST(WriteBatchReaderTest, TrustedReaderUsesAnOwnedBatchWithoutRevalidation) {
    EncodedWriteBatch batch;
    batch.SetSequence(77);
    batch.Put(AsBytes("alpha"), AsBytes("one"));
    batch.Delete(AsBytes("beta"));

    WriteBatchReader reader = WriteBatchReader::OpenTrusted(batch);

    EXPECT_EQ(reader.sequence(), 77U);
    EXPECT_EQ(reader.count(), 2U);
    const auto first = reader.Next();
    const auto second = reader.Next();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(AsStringView(first->key), "alpha");
    EXPECT_EQ(AsStringView(first->value), "one");
    EXPECT_EQ(first->sequence, 77U);
    EXPECT_EQ(AsStringView(second->key), "beta");
    EXPECT_EQ(second->kind, ValueKind::Deletion);
    EXPECT_EQ(second->sequence, 78U);
    EXPECT_FALSE(reader.Next().has_value());
}

TEST(WriteBatchReaderTest, TrustedMixedRecordsAdvanceConditionallyAndKeepBackingBytes) {
    const std::vector<std::byte> deleted_key = Pattern(128, 13);
    const std::vector<std::byte> put_key = Pattern(127, 17);
    const std::vector<std::byte> put_value = Pattern(128, 19);
    EncodedWriteBatch batch;
    batch.SetSequence(900);
    batch.Delete(deleted_key);
    batch.Put(put_key, put_value);
    const std::vector<std::byte> before = Materialize(batch.encoded());
    WriteBatchReader reader = WriteBatchReader::OpenTrusted(batch);

    const auto deleted = reader.Next();
    ASSERT_TRUE(deleted.has_value());
    EXPECT_EQ(deleted->sequence, 900U);
    EXPECT_EQ(deleted->kind, ValueKind::Deletion);
    EXPECT_TRUE(std::ranges::equal(deleted->key, deleted_key));
    EXPECT_TRUE(deleted->value.empty());
    EXPECT_EQ(deleted->value.data(), nullptr);
    EXPECT_GE(deleted->key.data(), batch.encoded().data());
    EXPECT_LT(deleted->key.data(), batch.encoded().data() + batch.encoded().size());

    const auto put = reader.Next();
    ASSERT_TRUE(put.has_value());
    EXPECT_EQ(put->sequence, 901U);
    EXPECT_EQ(put->kind, ValueKind::Value);
    EXPECT_TRUE(std::ranges::equal(put->key, put_key));
    EXPECT_TRUE(std::ranges::equal(put->value, put_value));
    EXPECT_GT(put->key.data(), deleted->key.data());
    EXPECT_GT(put->value.data(), put->key.data());

    EXPECT_FALSE(reader.Next().has_value());
    EXPECT_FALSE(reader.Next().has_value());
    EXPECT_EQ(Materialize(batch.encoded()), before);
}

TEST(WriteBatchTest, AppendPreservesDestinationSequenceAndSource) {
    EncodedWriteBatch destination;
    destination.SetSequence(100);
    destination.Put(AsBytes("a"), AsBytes("1"));

    EncodedWriteBatch source;
    source.SetSequence(9'999);
    source.Delete(AsBytes("b"));
    source.Put(AsBytes("c"), AsBytes("2"));
    const std::vector<std::byte> source_before = Materialize(source.encoded());

    destination.Append(source);

    EXPECT_EQ(destination.sequence(), 100U);
    EXPECT_EQ(destination.count(), 3U);
    EXPECT_EQ(Materialize(source.encoded()), source_before);

    auto opened = WriteBatchReader::Open(destination.encoded());
    ASSERT_TRUE(opened.has_value());
    WriteBatchReader reader = std::move(*opened);
    const auto first = reader.Next();
    const auto second = reader.Next();
    const auto third = reader.Next();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(AsStringView(first->key), "a");
    EXPECT_EQ(first->sequence, 100U);
    EXPECT_EQ(AsStringView(second->key), "b");
    EXPECT_EQ(second->sequence, 101U);
    EXPECT_EQ(AsStringView(third->key), "c");
    EXPECT_EQ(third->sequence, 102U);
}

TEST(WriteBatchTest, AppendHandlesEmptyAndSelfSources) {
    EncodedWriteBatch batch;
    batch.SetSequence(7);
    batch.Put(AsBytes("key"), AsBytes("value"));
    const std::vector<std::byte> before = Materialize(batch.encoded());

    const EncodedWriteBatch empty;
    batch.Append(empty);
    EXPECT_EQ(Materialize(batch.encoded()), before);

    batch.Append(batch);
    EXPECT_EQ(batch.sequence(), 7U);
    EXPECT_EQ(batch.count(), 2U);
    ASSERT_EQ(batch.encoded().size(), before.size() + before.size() - WriteBatchHeaderSize);
    EXPECT_TRUE(std::ranges::equal(
        batch.encoded().subspan(WriteBatchHeaderSize, before.size() - WriteBatchHeaderSize),
        batch.encoded().subspan(before.size())));
}

TEST(WriteBatchTest, MutationsLeaveSequenceAssignmentToTheCommitBoundary) {
    EncodedWriteBatch batch;
    batch.SetSequence(MaxSequenceNumber);
    batch.Put(AsBytes("last"), AsBytes("value"));

    EncodedWriteBatch source;
    source.Delete(AsBytes("penultimate"));
    batch.Append(source);

    EXPECT_EQ(batch.sequence(), MaxSequenceNumber);
    EXPECT_EQ(batch.count(), 2U);

    batch.SetSequence(MaxSequenceNumber - 1U);
    WriteBatchReader reader = WriteBatchReader::OpenTrusted(batch);
    ASSERT_TRUE(reader.Next().has_value());
    ASSERT_TRUE(reader.Next().has_value());
    EXPECT_FALSE(reader.Next().has_value());
}

TEST(WriteBatchTest, ClearRestoresCanonicalEmptyBatch) {
    EncodedWriteBatch batch;
    batch.SetSequence(88);
    batch.Put(AsBytes("key"), AsBytes("value"));

    batch.Clear();

    EXPECT_EQ(batch.sequence(), 0U);
    EXPECT_EQ(batch.count(), 0U);
    EXPECT_EQ(Materialize(batch.encoded()), std::vector<std::byte>(WriteBatchHeaderSize));
}

TEST(WriteBatchTest, CopyAndMovePreserveOwningInvariants) {
    EncodedWriteBatch original;
    original.SetSequence(55);
    original.Put(AsBytes("key"), AsBytes("value"));
    const std::vector<std::byte> expected = Materialize(original.encoded());

    EncodedWriteBatch copy = original;
    original.Clear();
    EXPECT_EQ(Materialize(copy.encoded()), expected);

    EncodedWriteBatch moved = std::move(copy);
    EXPECT_EQ(Materialize(moved.encoded()), expected);
    EXPECT_EQ(Materialize(copy.encoded()), std::vector<std::byte>(WriteBatchHeaderSize));
    EXPECT_EQ(copy.sequence(), 0U);
    EXPECT_EQ(copy.count(), 0U);

    EncodedWriteBatch assigned;
    assigned.Delete(AsBytes("old"));
    assigned = std::move(moved);
    EXPECT_EQ(Materialize(assigned.encoded()), expected);
    EXPECT_EQ(Materialize(moved.encoded()), std::vector<std::byte>(WriteBatchHeaderSize));

    MoveAssign(assigned, std::move(assigned));
    EXPECT_EQ(Materialize(assigned.encoded()), expected);
}

TEST(WriteBatchReaderTest, RejectsHeadersShorterThanTwelveBytes) {
    for (std::size_t size = 0; size < WriteBatchHeaderSize; ++size) {
        SCOPED_TRACE(size);
        ExpectCorruption(std::vector<std::byte>(size), "write batch is shorter than its header");
    }
}

TEST(WriteBatchReaderTest, RejectsUnknownAndTruncatedRecords) {
    const std::vector<std::pair<std::vector<std::byte>, std::string_view>> malformed{
        {Bytes({0x02}), "write batch has an unknown value kind"},
        {Bytes({0x01, 0x80}), "truncated varint"},
        {Bytes({0x00, 0x03, 0x61, 0x62}), "truncated length-prefixed value"},
        {Bytes({0x01, 0x00, 0x80}), "truncated varint"},
        {Bytes({0x01, 0x00, 0x03, 0x61, 0x62}), "truncated length-prefixed value"},
    };

    for (const auto& [records, message] : malformed) {
        SCOPED_TRACE(records.size());
        ExpectCorruption(EncodedBatch(0, 1, records), message);
    }
}

TEST(WriteBatchReaderTest, RejectsCountMismatchAndTrailingBytes) {
    ExpectCorruption(EncodedBatch(0, 1), "write batch count exceeds encoded records");
    ExpectCorruption(EncodedBatch(0, 0, Bytes({0x00, 0x00})), "write batch has trailing bytes");
    ExpectCorruption(EncodedBatch(0, 1, Bytes({0x00, 0x00, 0x00, 0x00})),
                     "write batch has trailing bytes");
    ExpectCorruption(EncodedBatch(0, 0, Bytes({0xff})), "write batch has trailing bytes");
}

TEST(WriteBatchReaderTest, PreservesRecordAndFieldFailurePriority) {
    ExpectCorruption(EncodedBatch(0, 2, Bytes({0x02, 0x80})),
                     "write batch has an unknown value kind");
    ExpectCorruption(EncodedBatch(0, 2, Bytes({0x01, 0x05, 'k', 0x80})),
                     "truncated length-prefixed value");
    ExpectCorruption(EncodedBatch(0, 2, Bytes({0x01, 0x00, 0x04, 0x02})),
                     "truncated length-prefixed value");
    ExpectCorruption(EncodedBatch(0, 1, Bytes({0x00, 0x00, 0x80})),
                     "write batch has trailing bytes");
    ExpectCorruption(EncodedBatch(0, 2, Bytes({0x00, 0x00})),
                     "write batch count exceeds encoded records");
}

TEST(WriteBatchReaderTest, PreservesLevelDbCompatibleUncheckedSequenceHeaders) {
    const std::vector<std::byte> empty_encoded = EncodedBatch(MaxSequenceNumber + 1U, 0);
    auto empty = WriteBatchReader::Open(empty_encoded);
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->sequence(), MaxSequenceNumber + 1U);

    const std::vector<std::byte> two_encoded =
        EncodedBatch(MaxSequenceNumber, 2, Bytes({0x00, 0x00, 0x00, 0x00}));
    auto two = WriteBatchReader::Open(two_encoded);
    ASSERT_TRUE(two.has_value());
    const auto first = two->Next();
    const auto second = two->Next();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->sequence, MaxSequenceNumber);
    EXPECT_EQ(second->sequence, MaxSequenceNumber + 1U);
}

TEST(WriteBatchReaderTest, AcceptsLevelDbCompatibleNonCanonicalVarints) {
    const std::vector<std::byte> encoded =
        EncodedBatch(3, 1, Bytes({0x01, 0x81, 0x00, 0x6b, 0x80, 0x00}));

    auto opened = WriteBatchReader::Open(encoded);

    ASSERT_TRUE(opened.has_value());
    WriteBatchReader reader = std::move(*opened);
    const auto entry = reader.Next();
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->sequence, 3U);
    EXPECT_EQ(AsStringView(entry->key), "k");
    EXPECT_TRUE(entry->value.empty());
    EXPECT_FALSE(reader.Next().has_value());
}

TEST(WriteBatchReaderTest, TruncatesTerminalLengthPayloadLikeLevelDb) {
    const std::vector<std::byte> encoded =
        EncodedBatch(4, 1, Bytes({0x00, 0x80, 0x80, 0x80, 0x80, 0x10}));

    auto opened = WriteBatchReader::Open(encoded);

    ASSERT_TRUE(opened.has_value());
    WriteBatchReader reader = std::move(*opened);
    const auto entry = reader.Next();
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->sequence, 4U);
    EXPECT_EQ(entry->kind, ValueKind::Deletion);
    EXPECT_TRUE(entry->key.empty());
    EXPECT_FALSE(reader.Next().has_value());
}

}  // namespace
}  // namespace modern_leveldb
