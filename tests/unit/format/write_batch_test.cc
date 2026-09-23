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

static_assert(std::is_copy_constructible_v<WriteBatch>);
static_assert(std::is_copy_assignable_v<WriteBatch>);
static_assert(std::is_move_constructible_v<WriteBatch>);
static_assert(std::is_move_assignable_v<WriteBatch>);
static_assert(noexcept(std::declval<WriteBatch&>().Clear()));
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

std::vector<std::byte> Materialize(ByteView value) {
  return {value.begin(), value.end()};
}

std::vector<std::byte> EncodedBatch(SequenceNumber sequence, std::uint32_t count,
                                    ByteView records = {}) {
  std::vector<std::byte> encoded;
  AppendFixed64(encoded, sequence);
  AppendFixed32(encoded, count);
  encoded.insert(encoded.end(), records.begin(), records.end());
  return encoded;
}

void ExpectCorruption(ByteView encoded) {
  const auto reader = WriteBatchReader::Open(encoded);
  ASSERT_FALSE(reader.has_value());
  EXPECT_EQ(reader.error().code(), ErrorCode::Corruption);
}

TEST(WriteBatchTest, PersistentConstantsAndDefaultHeaderMatchLevelDb) {
  EXPECT_EQ(WriteBatchHeaderSize, 12U);

  const WriteBatch batch;

  EXPECT_EQ(batch.sequence(), 0U);
  EXPECT_EQ(batch.count(), 0U);
  EXPECT_EQ(Materialize(batch.encoded()), std::vector<std::byte>(WriteBatchHeaderSize));
}

TEST(WriteBatchTest, MatchesLevelDbGoldenPutAndDeleteEncoding) {
  WriteBatch batch;
  ASSERT_TRUE(batch.SetSequence(0x00010203040506ULL).has_value());
  ASSERT_TRUE(batch.Put(AsBytes("foo"), AsBytes("bar")).has_value());
  ASSERT_TRUE(batch.Delete(AsBytes("box")).has_value());

  EXPECT_EQ(Materialize(batch.encoded()),
            Bytes({
                0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0x00,
                0x02, 0x00, 0x00, 0x00,
                0x01, 0x03, 0x66, 0x6f, 0x6f, 0x03, 0x62, 0x61, 0x72,
                0x00, 0x03, 0x62, 0x6f, 0x78,
            }));
}

TEST(WriteBatchTest, EncodesEmptyBinaryAndMultiByteLengths) {
  const std::array binary_key{std::byte{0x00}, std::byte{0xff}};
  const std::vector<std::byte> long_key(128, std::byte{'k'});
  WriteBatch batch;

  ASSERT_TRUE(batch.Put({}, {}).has_value());
  ASSERT_TRUE(batch.Delete(binary_key).has_value());
  ASSERT_TRUE(batch.Put(long_key, AsBytes("v")).has_value());

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

TEST(WriteBatchReaderTest, IteratesBorrowedEntriesWithAssignedSequences) {
  WriteBatch batch;
  ASSERT_TRUE(batch.SetSequence(100).has_value());
  ASSERT_TRUE(batch.Put(AsBytes("alpha"), AsBytes("one")).has_value());
  ASSERT_TRUE(batch.Delete(AsBytes("beta")).has_value());
  ASSERT_TRUE(batch.Put(AsBytes("gamma"), {}).has_value());

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

TEST(WriteBatchTest, AppendPreservesDestinationSequenceAndSource) {
  WriteBatch destination;
  ASSERT_TRUE(destination.SetSequence(100).has_value());
  ASSERT_TRUE(destination.Put(AsBytes("a"), AsBytes("1")).has_value());

  WriteBatch source;
  ASSERT_TRUE(source.SetSequence(9'999).has_value());
  ASSERT_TRUE(source.Delete(AsBytes("b")).has_value());
  ASSERT_TRUE(source.Put(AsBytes("c"), AsBytes("2")).has_value());
  const std::vector<std::byte> source_before = Materialize(source.encoded());

  ASSERT_TRUE(destination.Append(source).has_value());

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
  WriteBatch batch;
  ASSERT_TRUE(batch.SetSequence(7).has_value());
  ASSERT_TRUE(batch.Put(AsBytes("key"), AsBytes("value")).has_value());
  const std::vector<std::byte> before = Materialize(batch.encoded());

  const WriteBatch empty;
  ASSERT_TRUE(batch.Append(empty).has_value());
  EXPECT_EQ(Materialize(batch.encoded()), before);

  ASSERT_TRUE(batch.Append(batch).has_value());
  EXPECT_EQ(batch.sequence(), 7U);
  EXPECT_EQ(batch.count(), 2U);
  ASSERT_EQ(batch.encoded().size(), before.size() + before.size() - WriteBatchHeaderSize);
  EXPECT_TRUE(std::ranges::equal(
      batch.encoded().subspan(WriteBatchHeaderSize, before.size() - WriteBatchHeaderSize),
      batch.encoded().subspan(before.size())));
}

TEST(WriteBatchTest, SequenceValidationIsFailureAtomic) {
  WriteBatch batch;
  ASSERT_TRUE(batch.SetSequence(MaxSequenceNumber).has_value());
  ASSERT_TRUE(batch.Put(AsBytes("last"), AsBytes("value")).has_value());
  const std::vector<std::byte> one_record = Materialize(batch.encoded());

  const Status add_overflow = batch.Delete(AsBytes("overflow"));
  ASSERT_FALSE(add_overflow.has_value());
  EXPECT_EQ(add_overflow.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(Materialize(batch.encoded()), one_record);

  const Status set_too_large = batch.SetSequence(MaxSequenceNumber + 1U);
  ASSERT_FALSE(set_too_large.has_value());
  EXPECT_EQ(set_too_large.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(Materialize(batch.encoded()), one_record);

  ASSERT_TRUE(batch.SetSequence(MaxSequenceNumber - 1U).has_value());
  ASSERT_TRUE(batch.Delete(AsBytes("penultimate")).has_value());
  const std::vector<std::byte> two_records = Materialize(batch.encoded());

  const Status set_range_overflow = batch.SetSequence(MaxSequenceNumber);
  ASSERT_FALSE(set_range_overflow.has_value());
  EXPECT_EQ(set_range_overflow.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(Materialize(batch.encoded()), two_records);
}

TEST(WriteBatchTest, AppendSequenceValidationIsFailureAtomic) {
  WriteBatch destination;
  ASSERT_TRUE(destination.SetSequence(MaxSequenceNumber).has_value());
  ASSERT_TRUE(destination.Put(AsBytes("last"), {}).has_value());

  WriteBatch source;
  ASSERT_TRUE(source.Delete(AsBytes("too-much")).has_value());
  const std::vector<std::byte> before = Materialize(destination.encoded());

  const Status status = destination.Append(source);

  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(Materialize(destination.encoded()), before);
}

TEST(WriteBatchTest, ClearRestoresCanonicalEmptyBatch) {
  WriteBatch batch;
  ASSERT_TRUE(batch.SetSequence(88).has_value());
  ASSERT_TRUE(batch.Put(AsBytes("key"), AsBytes("value")).has_value());

  batch.Clear();

  EXPECT_EQ(batch.sequence(), 0U);
  EXPECT_EQ(batch.count(), 0U);
  EXPECT_EQ(Materialize(batch.encoded()), std::vector<std::byte>(WriteBatchHeaderSize));
}

TEST(WriteBatchTest, CopyAndMovePreserveOwningInvariants) {
  WriteBatch original;
  ASSERT_TRUE(original.SetSequence(55).has_value());
  ASSERT_TRUE(original.Put(AsBytes("key"), AsBytes("value")).has_value());
  const std::vector<std::byte> expected = Materialize(original.encoded());

  WriteBatch copy = original;
  original.Clear();
  EXPECT_EQ(Materialize(copy.encoded()), expected);

  WriteBatch moved = std::move(copy);
  EXPECT_EQ(Materialize(moved.encoded()), expected);
  EXPECT_EQ(Materialize(copy.encoded()), std::vector<std::byte>(WriteBatchHeaderSize));
  EXPECT_EQ(copy.sequence(), 0U);
  EXPECT_EQ(copy.count(), 0U);

  WriteBatch assigned;
  ASSERT_TRUE(assigned.Delete(AsBytes("old")).has_value());
  assigned = std::move(moved);
  EXPECT_EQ(Materialize(assigned.encoded()), expected);
  EXPECT_EQ(Materialize(moved.encoded()), std::vector<std::byte>(WriteBatchHeaderSize));
}

TEST(WriteBatchTest, MutationsSupportInputsAliasingEncodedStorage) {
  WriteBatch batch;
  ASSERT_TRUE(batch.Put(AsBytes("seed"), AsBytes("value")).has_value());

  std::optional<WriteBatchEntry> aliased;
  {
    auto opened = WriteBatchReader::Open(batch.encoded());
    ASSERT_TRUE(opened.has_value());
    WriteBatchReader reader = std::move(*opened);
    aliased = reader.Next();
    ASSERT_TRUE(aliased.has_value());
  }

  ASSERT_TRUE(batch.Put(aliased->key, aliased->value).has_value());

  {
    auto opened = WriteBatchReader::Open(batch.encoded());
    ASSERT_TRUE(opened.has_value());
    WriteBatchReader reader = std::move(*opened);
    aliased = reader.Next();
    ASSERT_TRUE(aliased.has_value());
  }
  ASSERT_TRUE(batch.Delete(aliased->key).has_value());

  auto opened = WriteBatchReader::Open(batch.encoded());
  ASSERT_TRUE(opened.has_value());
  WriteBatchReader reader = std::move(*opened);
  for (const ValueKind expected_kind :
       {ValueKind::Value, ValueKind::Value, ValueKind::Deletion}) {
    const auto entry = reader.Next();
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->kind, expected_kind);
    EXPECT_EQ(AsStringView(entry->key), "seed");
    if (expected_kind == ValueKind::Value) {
      EXPECT_EQ(AsStringView(entry->value), "value");
    }
  }
}

TEST(WriteBatchReaderTest, RejectsHeadersShorterThanTwelveBytes) {
  for (std::size_t size = 0; size < WriteBatchHeaderSize; ++size) {
    SCOPED_TRACE(size);
    ExpectCorruption(std::vector<std::byte>(size));
  }
}

TEST(WriteBatchReaderTest, RejectsUnknownAndTruncatedRecords) {
  const std::vector<std::vector<std::byte>> malformed{
      Bytes({0x02}),
      Bytes({0x01, 0x80}),
      Bytes({0x00, 0x03, 0x61, 0x62}),
      Bytes({0x01, 0x00, 0x80}),
      Bytes({0x01, 0x00, 0x03, 0x61, 0x62}),
  };

  for (const auto& records : malformed) {
    SCOPED_TRACE(records.size());
    ExpectCorruption(EncodedBatch(0, 1, records));
  }
}

TEST(WriteBatchReaderTest, RejectsCountMismatchAndTrailingBytes) {
  ExpectCorruption(EncodedBatch(0, 1));
  ExpectCorruption(EncodedBatch(0, 0, Bytes({0x00, 0x00})));
  ExpectCorruption(EncodedBatch(0, 1, Bytes({0x00, 0x00, 0x00, 0x00})));
  ExpectCorruption(EncodedBatch(0, 0, Bytes({0xff})));
}

TEST(WriteBatchReaderTest, RejectsSequenceRangeOverflow) {
  ExpectCorruption(EncodedBatch(MaxSequenceNumber + 1U, 0));
  ExpectCorruption(
      EncodedBatch(MaxSequenceNumber, 2, Bytes({0x00, 0x00, 0x00, 0x00})));
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
