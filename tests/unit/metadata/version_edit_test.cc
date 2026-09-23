#include "metadata/version_edit.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <string>
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

static_assert(std::is_copy_constructible_v<VersionEdit>);
static_assert(std::is_copy_assignable_v<VersionEdit>);
static_assert(std::is_move_constructible_v<VersionEdit>);
static_assert(std::is_move_assignable_v<VersionEdit>);

constexpr std::uint32_t ComparatorTag = 1;
constexpr std::uint32_t LogNumberTag = 2;
constexpr std::uint32_t NextFileNumberTag = 3;
constexpr std::uint32_t LastSequenceTag = 4;
constexpr std::uint32_t CompactPointerTag = 5;
constexpr std::uint32_t DeletedFileTag = 6;
constexpr std::uint32_t NewFileTag = 7;
constexpr std::uint32_t PrevLogNumberTag = 9;

std::vector<std::byte> Bytes(std::initializer_list<unsigned int> values) {
  std::vector<std::byte> result;
  result.reserve(values.size());
  for (const unsigned int value : values) {
    result.push_back(static_cast<std::byte>(value));
  }
  return result;
}

InternalKey Key(std::string_view user_key, SequenceNumber sequence,
                ValueKind kind = ValueKind::Value) {
  auto key = InternalKey::Create(AsBytes(user_key), sequence, kind);
  EXPECT_TRUE(key.has_value());
  return std::move(key).value();
}

FileMetadata File(std::uint64_t number, std::uint64_t file_size = 100) {
  return FileMetadata{
      .number = number,
      .file_size = file_size,
      .smallest = Key("a", 1),
      .largest = Key("z", 2),
  };
}

std::vector<std::byte> Materialize(ByteView value) {
  return std::vector<std::byte>(value.begin(), value.end());
}

void AppendKey(std::vector<std::byte>& encoded, ByteView key) {
  ASSERT_TRUE(AppendLengthPrefixed(encoded, key).has_value());
}

std::vector<std::byte> CompactPointerField(std::uint32_t level, ByteView key) {
  std::vector<std::byte> encoded;
  AppendVarint32(encoded, CompactPointerTag);
  AppendVarint32(encoded, level);
  AppendKey(encoded, key);
  return encoded;
}

std::vector<std::byte> DeletedFileField(std::uint32_t level, std::uint64_t number) {
  std::vector<std::byte> encoded;
  AppendVarint32(encoded, DeletedFileTag);
  AppendVarint32(encoded, level);
  AppendVarint64(encoded, number);
  return encoded;
}

std::vector<std::byte> NewFileField(std::uint32_t level, std::uint64_t number, ByteView smallest,
                                    ByteView largest) {
  std::vector<std::byte> encoded;
  AppendVarint32(encoded, NewFileTag);
  AppendVarint32(encoded, level);
  AppendVarint64(encoded, number);
  AppendVarint64(encoded, 100);
  AppendKey(encoded, smallest);
  AppendKey(encoded, largest);
  return encoded;
}

std::vector<std::byte> NumberField(std::uint32_t tag, std::uint64_t number) {
  std::vector<std::byte> encoded;
  AppendVarint32(encoded, tag);
  AppendVarint64(encoded, number);
  return encoded;
}

std::vector<std::byte> Concat(std::initializer_list<std::vector<std::byte>> parts) {
  std::vector<std::byte> result;
  for (const std::vector<std::byte>& part : parts) {
    result.insert(result.end(), part.begin(), part.end());
  }
  return result;
}

VersionEdit Decoded(ByteView encoded) {
  auto decoded = VersionEdit::Decode(encoded);
  EXPECT_TRUE(decoded.has_value());
  return std::move(decoded).value();
}

void ExpectCorruption(ByteView encoded, std::string_view field) {
  SCOPED_TRACE(field);
  const Result<VersionEdit> decoded = VersionEdit::Decode(encoded);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
  EXPECT_NE(decoded.error().message().find(field), std::string_view::npos)
      << decoded.error().message();
}

void ExpectInvalidArgument(const Status& status) {
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
}

void ExpectSameKey(const InternalKey& actual, const InternalKey& expected) {
  EXPECT_EQ(Materialize(actual.encoded()), Materialize(expected.encoded()));
}

// Moving an internal key leaves an empty encoding, which is not a valid key.
InternalKey MovedFromKey() {
  InternalKey source = Key("m", 3);
  [[maybe_unused]] const InternalKey destination(std::move(source));
  return source;  // NOLINT(bugprone-use-after-move): the moved-from state is under test.
}

TEST(VersionEditTest, EmptyEditEncodesToNothing) {
  const VersionEdit edit;

  EXPECT_TRUE(edit.Encode().empty());

  const VersionEdit decoded = Decoded({});
  EXPECT_FALSE(decoded.comparator_name().has_value());
  EXPECT_FALSE(decoded.log_number().has_value());
  EXPECT_FALSE(decoded.prev_log_number().has_value());
  EXPECT_FALSE(decoded.next_file_number().has_value());
  EXPECT_FALSE(decoded.last_sequence().has_value());
  EXPECT_TRUE(decoded.compact_pointers().empty());
  EXPECT_TRUE(decoded.deleted_files().empty());
  EXPECT_TRUE(decoded.new_files().empty());
}

TEST(VersionEditTest, EncodesEveryFieldInLevelDbOrder) {
  VersionEdit edit;
  ASSERT_TRUE(edit.AddFile(3,
                           FileMetadata{
                               .number = 12,
                               .file_size = 1000,
                               .smallest = Key("b", 1, ValueKind::Value),
                               .largest = Key("c", 2, ValueKind::Deletion),
                           })
                  .has_value());
  ASSERT_TRUE(edit.RemoveFile(2, 11).has_value());
  ASSERT_TRUE(edit.AddCompactPointer(1, Key("a", 3)).has_value());
  ASSERT_TRUE(edit.SetLastSequence(9).has_value());
  ASSERT_TRUE(edit.SetNextFileNumber(7).has_value());
  edit.SetPrevLogNumber(4);
  edit.SetLogNumber(5);
  ASSERT_TRUE(edit.SetComparatorName("cmp").has_value());

  // clang-format off
  EXPECT_EQ(edit.Encode(),
            Bytes({
                0x01, 0x03, 'c', 'm', 'p',
                0x02, 0x05,
                0x09, 0x04,
                0x03, 0x07,
                0x04, 0x09,
                0x05, 0x01, 0x09, 'a', 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x06, 0x02, 0x0b,
                0x07, 0x03, 0x0c, 0xe8, 0x07,
                0x09, 'b', 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x09, 'c', 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            }));
  // clang-format on
}

TEST(VersionEditTest, RoundTripsEveryField) {
  constexpr std::uint64_t Big = std::uint64_t{1} << 50U;

  VersionEdit edit;
  for (std::uint32_t index = 0; index < 4; ++index) {
    ASSERT_TRUE(edit.AddFile(3,
                             FileMetadata{
                                 .number = Big + 300 + index,
                                 .file_size = Big + 400 + index,
                                 .smallest = Key("foo", Big + 500 + index),
                                 .largest = Key("zoo", Big + 600 + index, ValueKind::Deletion),
                             })
                    .has_value());
    ASSERT_TRUE(edit.RemoveFile(4, Big + 700 + index).has_value());
    ASSERT_TRUE(edit.AddCompactPointer(index, Key("x", Big + 900 + index)).has_value());
  }
  ASSERT_TRUE(edit.SetComparatorName("foo").has_value());
  edit.SetLogNumber(Big + 100);
  edit.SetPrevLogNumber(Big + 99);
  ASSERT_TRUE(edit.SetNextFileNumber(Big + 200).has_value());
  ASSERT_TRUE(edit.SetLastSequence(Big + 1000).has_value());

  const std::vector<std::byte> encoded = edit.Encode();
  const VersionEdit decoded = Decoded(encoded);

  EXPECT_EQ(decoded.Encode(), encoded);
  EXPECT_EQ(decoded.comparator_name(), "foo");
  EXPECT_EQ(decoded.log_number(), Big + 100);
  EXPECT_EQ(decoded.prev_log_number(), Big + 99);
  EXPECT_EQ(decoded.next_file_number(), Big + 200);
  EXPECT_EQ(decoded.last_sequence(), Big + 1000);

  ASSERT_EQ(decoded.compact_pointers().size(), 4U);
  ASSERT_EQ(decoded.new_files().size(), 4U);
  ASSERT_EQ(decoded.deleted_files().size(), 4U);
  auto deleted = decoded.deleted_files().begin();
  for (std::uint32_t index = 0; index < 4; ++index, ++deleted) {
    const CompactPointer& pointer = decoded.compact_pointers()[index];
    EXPECT_EQ(pointer.level, index);
    ExpectSameKey(pointer.key, Key("x", Big + 900 + index));

    const NewFile& added = decoded.new_files()[index];
    EXPECT_EQ(added.level, 3U);
    EXPECT_EQ(added.file.number, Big + 300 + index);
    EXPECT_EQ(added.file.file_size, Big + 400 + index);
    ExpectSameKey(added.file.smallest, Key("foo", Big + 500 + index));
    ExpectSameKey(added.file.largest, Key("zoo", Big + 600 + index, ValueKind::Deletion));

    EXPECT_EQ(*deleted, (DeletedFile{.level = 4, .number = Big + 700 + index}));
  }
}

TEST(VersionEditTest, RoundTripsBoundaryValues) {
  constexpr std::uint64_t MaxNumber = std::numeric_limits<std::uint64_t>::max();

  VersionEdit edit;
  ASSERT_TRUE(edit.SetComparatorName("").has_value());
  edit.SetLogNumber(0);
  edit.SetPrevLogNumber(0);
  ASSERT_TRUE(edit.SetNextFileNumber(MaxNumber).has_value());
  ASSERT_TRUE(edit.SetLastSequence(MaxSequenceNumber).has_value());
  ASSERT_TRUE(edit.AddCompactPointer(NumLevels - 1, Key("", MaxSequenceNumber)).has_value());
  ASSERT_TRUE(edit.RemoveFile(NumLevels - 1, MaxNumber).has_value());
  ASSERT_TRUE(edit.AddFile(NumLevels - 1, File(MaxNumber, MaxNumber)).has_value());

  const std::vector<std::byte> encoded = edit.Encode();
  const VersionEdit decoded = Decoded(encoded);

  EXPECT_EQ(decoded.Encode(), encoded);
  EXPECT_EQ(decoded.comparator_name(), "");
  EXPECT_EQ(decoded.log_number(), 0U);
  EXPECT_EQ(decoded.prev_log_number(), 0U);
  EXPECT_EQ(decoded.next_file_number(), MaxNumber);
  EXPECT_EQ(decoded.last_sequence(), MaxSequenceNumber);
}

TEST(VersionEditTest, LaterScalarValuesReplaceEarlierOnes) {
  VersionEdit edit;
  ASSERT_TRUE(edit.SetComparatorName("old").has_value());
  ASSERT_TRUE(edit.SetComparatorName("new").has_value());
  edit.SetLogNumber(1);
  edit.SetLogNumber(2);
  edit.SetPrevLogNumber(3);
  edit.SetPrevLogNumber(4);
  ASSERT_TRUE(edit.SetNextFileNumber(5).has_value());
  ASSERT_TRUE(edit.SetNextFileNumber(6).has_value());
  ASSERT_TRUE(edit.SetLastSequence(7).has_value());
  ASSERT_TRUE(edit.SetLastSequence(8).has_value());

  EXPECT_EQ(edit.comparator_name(), "new");
  EXPECT_EQ(edit.log_number(), 2U);
  EXPECT_EQ(edit.prev_log_number(), 4U);
  EXPECT_EQ(edit.next_file_number(), 6U);
  EXPECT_EQ(edit.last_sequence(), 8U);
  EXPECT_EQ(edit.Encode(),
            Bytes({0x01, 0x03, 'n', 'e', 'w', 0x02, 0x02, 0x09, 0x04, 0x03, 0x06, 0x04, 0x08}));
}

TEST(VersionEditTest, ComparatorNameMayAliasTheCurrentName) {
  const std::string name = std::string(64, 'n') + "suffix";
  VersionEdit edit;
  ASSERT_TRUE(edit.SetComparatorName(name).has_value());

  ASSERT_TRUE(edit.SetComparatorName(*edit.comparator_name()).has_value());
  EXPECT_EQ(edit.comparator_name(), name);

  ASSERT_TRUE(
      edit.SetComparatorName(std::string_view(*edit.comparator_name()).substr(8)).has_value());
  EXPECT_EQ(edit.comparator_name(), name.substr(8));
}

TEST(VersionEditTest, DeletedFilesOrderByLevelThenNumber) {
  const DeletedFile file{.level = 1, .number = 5};

  EXPECT_EQ(file, (DeletedFile{.level = 1, .number = 5}));
  EXPECT_NE(file, (DeletedFile{.level = 1, .number = 6}));
  EXPECT_NE(file, (DeletedFile{.level = 2, .number = 5}));
  EXPECT_LT(file, (DeletedFile{.level = 1, .number = 6}));
  EXPECT_LT(file, (DeletedFile{.level = 2, .number = 1}));
  EXPECT_GT(file, (DeletedFile{.level = 1, .number = 4}));
  EXPECT_GT(file, (DeletedFile{.level = 0, .number = 9}));
}

TEST(VersionEditTest, DeletedFilesAreSortedAndDeduplicated) {
  VersionEdit edit;
  ASSERT_TRUE(edit.RemoveFile(2, 5).has_value());
  ASSERT_TRUE(edit.RemoveFile(1, 9).has_value());
  ASSERT_TRUE(edit.RemoveFile(2, 5).has_value());
  ASSERT_TRUE(edit.RemoveFile(1, 3).has_value());

  EXPECT_EQ(edit.deleted_files(), (std::set<DeletedFile>{
                                      {.level = 1, .number = 3},
                                      {.level = 1, .number = 9},
                                      {.level = 2, .number = 5},
                                  }));
  EXPECT_EQ(edit.Encode(), Bytes({0x06, 0x01, 0x03, 0x06, 0x01, 0x09, 0x06, 0x02, 0x05}));
}

TEST(VersionEditTest, CompactPointersAndNewFilesKeepInsertionOrder) {
  VersionEdit edit;
  ASSERT_TRUE(edit.AddCompactPointer(2, Key("b", 1)).has_value());
  ASSERT_TRUE(edit.AddCompactPointer(1, Key("a", 2)).has_value());
  ASSERT_TRUE(edit.AddCompactPointer(2, Key("c", 3)).has_value());
  ASSERT_TRUE(edit.AddFile(1, File(20)).has_value());
  ASSERT_TRUE(edit.AddFile(0, File(10)).has_value());
  ASSERT_TRUE(edit.AddFile(1, File(20)).has_value());

  ASSERT_EQ(edit.compact_pointers().size(), 3U);
  EXPECT_EQ(edit.compact_pointers()[0].level, 2U);
  ExpectSameKey(edit.compact_pointers()[0].key, Key("b", 1));
  EXPECT_EQ(edit.compact_pointers()[1].level, 1U);
  ExpectSameKey(edit.compact_pointers()[1].key, Key("a", 2));
  EXPECT_EQ(edit.compact_pointers()[2].level, 2U);
  ExpectSameKey(edit.compact_pointers()[2].key, Key("c", 3));

  ASSERT_EQ(edit.new_files().size(), 3U);
  EXPECT_EQ(edit.new_files()[0].level, 1U);
  EXPECT_EQ(edit.new_files()[0].file.number, 20U);
  EXPECT_EQ(edit.new_files()[1].level, 0U);
  EXPECT_EQ(edit.new_files()[1].file.number, 10U);
  EXPECT_EQ(edit.new_files()[2].level, 1U);
  EXPECT_EQ(edit.new_files()[2].file.number, 20U);

  const VersionEdit decoded = Decoded(edit.Encode());
  EXPECT_EQ(decoded.Encode(), edit.Encode());
}

TEST(VersionEditTest, RejectsInvalidMutationsWithoutChangingTheEdit) {
  VersionEdit edit;
  ASSERT_TRUE(edit.SetNextFileNumber(5).has_value());
  ASSERT_TRUE(edit.SetLastSequence(6).has_value());
  ASSERT_TRUE(edit.AddCompactPointer(1, Key("a", 1)).has_value());
  ASSERT_TRUE(edit.RemoveFile(1, 2).has_value());
  ASSERT_TRUE(edit.AddFile(2, File(3)).has_value());
  const std::vector<std::byte> original = edit.Encode();

  ExpectInvalidArgument(edit.SetNextFileNumber(0));
  ExpectInvalidArgument(edit.SetLastSequence(MaxSequenceNumber + 1));
  ExpectInvalidArgument(edit.AddCompactPointer(NumLevels, Key("b", 2)));
  ExpectInvalidArgument(edit.AddCompactPointer(1, MovedFromKey()));
  ExpectInvalidArgument(edit.RemoveFile(NumLevels, 4));
  ExpectInvalidArgument(edit.RemoveFile(0, 0));
  ExpectInvalidArgument(edit.AddFile(NumLevels, File(4)));
  ExpectInvalidArgument(edit.AddFile(0, File(0)));
  FileMetadata malformed_smallest = File(4);
  malformed_smallest.smallest = MovedFromKey();
  ExpectInvalidArgument(edit.AddFile(0, std::move(malformed_smallest)));
  FileMetadata malformed_largest = File(4);
  malformed_largest.largest = MovedFromKey();
  ExpectInvalidArgument(edit.AddFile(0, std::move(malformed_largest)));

  EXPECT_EQ(edit.next_file_number(), 5U);
  EXPECT_EQ(edit.last_sequence(), 6U);
  EXPECT_EQ(edit.compact_pointers().size(), 1U);
  EXPECT_EQ(edit.deleted_files().size(), 1U);
  EXPECT_EQ(edit.new_files().size(), 1U);
  EXPECT_EQ(edit.Encode(), original);
}

TEST(VersionEditTest, DecodingCanonicalizesFieldOrder) {
  const std::vector<std::byte> smallest = Materialize(Key("a", 1).encoded());
  const std::vector<std::byte> largest = Materialize(Key("z", 2).encoded());
  const std::vector<std::byte> pointer = Materialize(Key("m", 3).encoded());
  const std::vector<std::byte> encoded = Concat({
      NewFileField(2, 8, smallest, largest),
      DeletedFileField(3, 9),
      DeletedFileField(1, 7),
      CompactPointerField(4, pointer),
      NumberField(PrevLogNumberTag, 4),
      NumberField(LastSequenceTag, 6),
      NumberField(NextFileNumberTag, 5),
      NumberField(LogNumberTag, 3),
      Bytes({ComparatorTag, 0x01, 'c'}),
  });

  const VersionEdit decoded = Decoded(encoded);

  EXPECT_EQ(decoded.Encode(), Concat({
                                  Bytes({ComparatorTag, 0x01, 'c'}),
                                  NumberField(LogNumberTag, 3),
                                  NumberField(PrevLogNumberTag, 4),
                                  NumberField(NextFileNumberTag, 5),
                                  NumberField(LastSequenceTag, 6),
                                  CompactPointerField(4, pointer),
                                  DeletedFileField(1, 7),
                                  DeletedFileField(3, 9),
                                  NewFileField(2, 8, smallest, largest),
                              }));
}

TEST(VersionEditTest, DecodingKeepsTheLastRepeatedScalarValue) {
  const VersionEdit decoded = Decoded(Concat({
      Bytes({ComparatorTag, 0x01, 'a'}),
      NumberField(LogNumberTag, 1),
      NumberField(PrevLogNumberTag, 2),
      NumberField(NextFileNumberTag, 3),
      NumberField(LastSequenceTag, 4),
      Bytes({ComparatorTag, 0x01, 'b'}),
      NumberField(LogNumberTag, 5),
      NumberField(PrevLogNumberTag, 6),
      NumberField(NextFileNumberTag, 7),
      NumberField(LastSequenceTag, 8),
  }));

  EXPECT_EQ(decoded.comparator_name(), "b");
  EXPECT_EQ(decoded.log_number(), 5U);
  EXPECT_EQ(decoded.prev_log_number(), 6U);
  EXPECT_EQ(decoded.next_file_number(), 7U);
  EXPECT_EQ(decoded.last_sequence(), 8U);
}

TEST(VersionEditTest, DecodingCollapsesRepeatedDeletedFiles) {
  const VersionEdit decoded = Decoded(Concat({DeletedFileField(1, 7), DeletedFileField(1, 7)}));

  EXPECT_EQ(decoded.deleted_files(), (std::set<DeletedFile>{{.level = 1, .number = 7}}));
}

TEST(VersionEditTest, EveryTruncatedPrefixDecodesExactlyOrReportsCorruption) {
  VersionEdit edit;
  std::set<std::size_t> field_boundaries = {0};
  const auto record_boundary = [&] { field_boundaries.insert(edit.Encode().size()); };
  ASSERT_TRUE(edit.SetComparatorName("comparator").has_value());
  record_boundary();
  edit.SetLogNumber(300);
  record_boundary();
  edit.SetPrevLogNumber(299);
  record_boundary();
  ASSERT_TRUE(edit.SetNextFileNumber(301).has_value());
  record_boundary();
  ASSERT_TRUE(edit.SetLastSequence(1'000'000).has_value());
  record_boundary();
  ASSERT_TRUE(edit.AddCompactPointer(1, Key("pointer", 200)).has_value());
  record_boundary();
  ASSERT_TRUE(edit.RemoveFile(2, 400).has_value());
  record_boundary();
  ASSERT_TRUE(edit.AddFile(3, File(500, 1'000'000)).has_value());
  record_boundary();

  const std::vector<std::byte> encoded = edit.Encode();
  for (std::size_t length = 0; length <= encoded.size(); ++length) {
    SCOPED_TRACE(length);
    const ByteView prefix = ByteView(encoded).first(length);
    const Result<VersionEdit> decoded = VersionEdit::Decode(prefix);
    if (field_boundaries.contains(length)) {
      ASSERT_TRUE(decoded.has_value());
      EXPECT_EQ(decoded->Encode(), Materialize(prefix));
    } else {
      ASSERT_FALSE(decoded.has_value());
      EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
    }
  }
}

TEST(VersionEditTest, RejectsMalformedAndUnknownTags) {
  ExpectCorruption(Bytes({0x80}), "tag");
  ExpectCorruption(Bytes({0x00}), "tag");
  ExpectCorruption(Bytes({0x08, 0x00}), "tag");
  ExpectCorruption(Bytes({0x0a, 0x00}), "tag");
  ExpectCorruption(Bytes({0x80, 0x01}), "tag");
  ExpectCorruption(Concat({NumberField(LogNumberTag, 1), Bytes({0x08})}), "tag");
}

TEST(VersionEditTest, RejectsOutOfRangeLevels) {
  const std::vector<std::byte> key = Materialize(Key("a", 1).encoded());

  ExpectCorruption(CompactPointerField(NumLevels, key), "compact pointer");
  ExpectCorruption(DeletedFileField(NumLevels, 1), "deleted file");
  ExpectCorruption(NewFileField(NumLevels, 1, key, key), "new file");
}

TEST(VersionEditTest, RejectsZeroFileNumbers) {
  const std::vector<std::byte> key = Materialize(Key("a", 1).encoded());

  ExpectCorruption(NumberField(NextFileNumberTag, 0), "next file number");
  ExpectCorruption(DeletedFileField(0, 0), "deleted file");
  ExpectCorruption(NewFileField(0, 0, key, key), "new file");
}

TEST(VersionEditTest, RejectsOutOfRangeLastSequence) {
  ExpectCorruption(NumberField(LastSequenceTag, MaxSequenceNumber + 1), "last sequence");
}

TEST(VersionEditTest, RejectsMalformedInternalKeys) {
  const std::vector<std::byte> valid = Materialize(Key("a", 1).encoded());
  const std::vector<std::byte> short_key = Bytes({'a', 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00});
  const std::vector<std::byte> unknown_kind =
      Bytes({'a', 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

  ExpectCorruption(CompactPointerField(1, short_key), "compact pointer");
  ExpectCorruption(CompactPointerField(1, unknown_kind), "compact pointer");
  ExpectCorruption(NewFileField(1, 1, short_key, valid), "new file");
  ExpectCorruption(NewFileField(1, 1, valid, unknown_kind), "new file");
}

TEST(VersionEditTest, ReportsTheFieldOfATruncatedPayload) {
  ExpectCorruption(Bytes({ComparatorTag, 0x02, 'a'}), "comparator name");
  ExpectCorruption(Bytes({LogNumberTag}), "log number");
  ExpectCorruption(Bytes({PrevLogNumberTag, 0x80}), "previous log number");
  ExpectCorruption(Bytes({NextFileNumberTag}), "next file number");
  ExpectCorruption(Bytes({LastSequenceTag, 0xff}), "last sequence");
  ExpectCorruption(Bytes({CompactPointerTag, 0x01}), "compact pointer");
  ExpectCorruption(Bytes({DeletedFileTag, 0x01}), "deleted file");
  ExpectCorruption(Bytes({NewFileTag, 0x01, 0x01, 0x01}), "new file");
}

}  // namespace
}  // namespace modern_leveldb
