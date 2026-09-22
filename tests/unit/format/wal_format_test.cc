#include "format/wal_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/crc32c.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

static_assert(!std::is_copy_constructible_v<WalFragmenter>);
static_assert(!std::is_copy_assignable_v<WalFragmenter>);
static_assert(!std::is_move_constructible_v<WalFragmenter>);
static_assert(!std::is_move_assignable_v<WalFragmenter>);

std::vector<std::byte> Bytes(std::initializer_list<unsigned int> values) {
  std::vector<std::byte> result;
  result.reserve(values.size());
  for (const unsigned int value : values) {
    result.push_back(static_cast<std::byte>(value));
  }
  return result;
}

std::vector<std::byte> Materialize(const std::vector<WalFragment>& fragments) {
  std::vector<std::byte> result;
  for (const WalFragment& fragment : fragments) {
    result.insert(result.end(), fragment.padding_before, std::byte{0});
    result.insert(result.end(), fragment.header.begin(), fragment.header.end());
    result.insert(result.end(), fragment.payload.begin(), fragment.payload.end());
  }
  return result;
}

std::vector<std::byte> PhysicalRecord(std::uint8_t type, ByteView payload,
                                      bool valid_checksum = true) {
  std::vector<std::byte> result;
  const std::byte type_byte = static_cast<std::byte>(type);
  std::uint32_t crc = ExtendCrc32c(Crc32c(ByteView(&type_byte, 1)), payload);
  crc = MaskCrc32c(crc);
  if (!valid_checksum) {
    ++crc;
  }
  AppendFixed32(result, crc);
  result.push_back(static_cast<std::byte>(payload.size() & 0xffU));
  result.push_back(static_cast<std::byte>((payload.size() >> 8U) & 0xffU));
  result.push_back(type_byte);
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}

std::vector<std::byte> Pattern(std::size_t size) {
  std::vector<std::byte> result(size);
  for (std::size_t index = 0; index < size; ++index) {
    result[index] = static_cast<std::byte>((index * 29U + 7U) & 0xffU);
  }
  return result;
}

TEST(WalFormatTest, PersistentConstantsMatchLevelDb) {
  EXPECT_EQ(WalBlockSize, 32U * 1'024U);
  EXPECT_EQ(WalHeaderSize, 7U);
  EXPECT_EQ(static_cast<std::uint8_t>(WalRecordType::Zero), 0U);
  EXPECT_EQ(static_cast<std::uint8_t>(WalRecordType::Full), 1U);
  EXPECT_EQ(static_cast<std::uint8_t>(WalRecordType::First), 2U);
  EXPECT_EQ(static_cast<std::uint8_t>(WalRecordType::Middle), 3U);
  EXPECT_EQ(static_cast<std::uint8_t>(WalRecordType::Last), 4U);
}

TEST(WalFragmenterTest, MatchesLevelDbEmptyAndSmallGoldenRecords) {
  WalFragmenter fragmenter;

  EXPECT_EQ(Materialize(fragmenter.Fragment({})),
            Bytes({0x05, 0x2b, 0x28, 0x43, 0x00, 0x00, 0x01}));
  EXPECT_EQ(Materialize(fragmenter.Fragment(AsBytes("foo"))),
            Bytes({0xdd, 0x5f, 0xb3, 0x7a, 0x03, 0x00, 0x01, 0x66, 0x6f, 0x6f}));
  EXPECT_EQ(fragmenter.block_offset(), 17U);
}

TEST(WalFragmenterTest, PadsShortBlockTrailerFromInitialOffset) {
  WalFragmenter fragmenter(WalBlockSize - 3U);

  const auto fragments = fragmenter.Fragment(AsBytes("a"));

  ASSERT_EQ(fragments.size(), 1U);
  EXPECT_EQ(fragments[0].padding_before, 3U);
  EXPECT_EQ(Materialize(fragments),
            Bytes({0x00, 0x00, 0x00, 0xb5, 0xcd, 0x0b, 0xa2, 0x01, 0x00, 0x01, 0x61}));
  EXPECT_EQ(fragmenter.block_offset(), WalHeaderSize + 1U);
}

TEST(WalFragmenterTest, PadsEveryShortBlockTrailer) {
  for (std::size_t trailer = 1; trailer < WalHeaderSize; ++trailer) {
    SCOPED_TRACE(trailer);
    WalFragmenter fragmenter(WalBlockSize - trailer);

    const auto fragments = fragmenter.Fragment(AsBytes("x"));

    ASSERT_EQ(fragments.size(), 1U);
    EXPECT_EQ(fragments[0].padding_before, trailer);
    EXPECT_EQ(fragmenter.block_offset(), WalHeaderSize + 1U);
  }
}

TEST(WalFragmenterTest, PreservesExactSevenByteLevelDbBoundaryBehavior) {
  WalFragmenter fragmenter(WalBlockSize - WalHeaderSize);

  const auto fragments = fragmenter.Fragment(AsBytes("bar"));

  ASSERT_EQ(fragments.size(), 2U);
  EXPECT_EQ(fragments[0].padding_before, 0U);
  EXPECT_EQ(fragments[0].payload.size(), 0U);
  EXPECT_EQ(fragments[0].header[6], static_cast<std::byte>(WalRecordType::First));
  EXPECT_TRUE(std::ranges::equal(fragments[1].payload, AsBytes("bar")));
  EXPECT_EQ(fragments[1].header[6], static_cast<std::byte>(WalRecordType::Last));
  EXPECT_EQ(Materialize(fragments), Bytes({0x64, 0x51, 0xd0, 0xe9, 0x00, 0x00, 0x02, 0x05, 0x9e,
                                           0x81, 0x37, 0x03, 0x00, 0x04, 0x62, 0x61, 0x72}));
  EXPECT_EQ(fragmenter.block_offset(), WalHeaderSize + 3U);
}

TEST(WalFragmenterTest, NormalizesExactlyFullBlockToZeroOffset) {
  const std::vector<std::byte> logical_record = Pattern(WalBlockSize - WalHeaderSize);
  WalFragmenter fragmenter;

  const auto fragments = fragmenter.Fragment(logical_record);

  ASSERT_EQ(fragments.size(), 1U);
  EXPECT_EQ(fragments[0].header[6], static_cast<std::byte>(WalRecordType::Full));
  EXPECT_EQ(fragmenter.block_offset(), 0U);
}

TEST(WalFragmenterTest, FragmentsLargeRecordsWithoutCopyingPayload) {
  const std::vector<std::byte> logical_record = Pattern(2U * WalBlockSize + 100U);
  WalFragmenter fragmenter;

  const auto fragments = fragmenter.Fragment(logical_record);

  ASSERT_EQ(fragments.size(), 3U);
  EXPECT_EQ(fragments[0].header[6], static_cast<std::byte>(WalRecordType::First));
  EXPECT_EQ(fragments[1].header[6], static_cast<std::byte>(WalRecordType::Middle));
  EXPECT_EQ(fragments[2].header[6], static_cast<std::byte>(WalRecordType::Last));
  EXPECT_EQ(fragments[0].payload.size(), WalBlockSize - WalHeaderSize);
  EXPECT_EQ(fragments[1].payload.size(), WalBlockSize - WalHeaderSize);
  EXPECT_EQ(fragments[0].payload.data(), logical_record.data());
  EXPECT_EQ(fragments[1].payload.data(), logical_record.data() + fragments[0].payload.size());

  std::vector<std::byte> reconstructed;
  for (const auto& fragment : fragments) {
    reconstructed.insert(reconstructed.end(), fragment.payload.begin(), fragment.payload.end());
  }
  EXPECT_EQ(reconstructed, logical_record);
}

TEST(WalFragmenterTest, NormalizesInitialFileSizeToBlockOffset) {
  WalFragmenter fragmenter(9U * WalBlockSize + 123U);

  EXPECT_EQ(fragmenter.block_offset(), 123U);
}

TEST(WalDecoderTest, DecodesFullRecordAndBorrowsPayload) {
  const std::vector<std::byte> encoded =
      Bytes({0xdd, 0x5f, 0xb3, 0x7a, 0x03, 0x00, 0x01, 0x66, 0x6f, 0x6f, 0xaa});

  const auto decoded = DecodeWalFragment(encoded);

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->kind, WalDecodeKind::Fragment);
  EXPECT_EQ(decoded->type, WalRecordType::Full);
  EXPECT_EQ(AsStringView(decoded->payload), "foo");
  EXPECT_EQ(decoded->payload.data(), encoded.data() + WalHeaderSize);
  EXPECT_EQ(decoded->encoded_size, WalHeaderSize + 3U);
}

TEST(WalDecoderTest, DecodesEveryFragmentType) {
  for (const WalRecordType type :
       {WalRecordType::Full, WalRecordType::First, WalRecordType::Middle, WalRecordType::Last}) {
    SCOPED_TRACE(static_cast<int>(type));
    const auto encoded = PhysicalRecord(static_cast<std::uint8_t>(type), AsBytes("fragment"));

    const auto decoded = DecodeWalFragment(encoded);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->kind, WalDecodeKind::Fragment);
    EXPECT_EQ(decoded->type, type);
    EXPECT_EQ(AsStringView(decoded->payload), "fragment");
  }
}

TEST(WalDecoderTest, ZeroMarkerEndsTheCurrentBlockWithoutChecksum) {
  const std::vector<std::byte> encoded{std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
                                       std::byte{0xff}, std::byte{0x00}, std::byte{0x00},
                                       std::byte{0x00}, std::byte{0x01}};

  const auto decoded = DecodeWalFragment(encoded);

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->kind, WalDecodeKind::EndOfBlock);
  EXPECT_EQ(decoded->type, WalRecordType::Zero);
  EXPECT_TRUE(decoded->payload.empty());
  EXPECT_EQ(decoded->encoded_size, WalHeaderSize);
}

TEST(WalDecoderTest, ReportsTrustedUnknownTypeAsSkippable) {
  const std::vector<std::byte> encoded = PhysicalRecord(9, AsBytes("unknown"));

  const auto decoded = DecodeWalFragment(encoded);

  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().failure, WalDecodeFailure::UnknownType);
  EXPECT_EQ(decoded.error().recovery, WalRecoveryAction::SkipPhysicalRecord);
  EXPECT_EQ(decoded.error().encoded_size, encoded.size());
  EXPECT_EQ(decoded.error().error.code(), ErrorCode::Corruption);
}

TEST(WalDecoderTest, BadChecksumOnUnknownTypeDropsBlock) {
  const std::vector<std::byte> encoded = PhysicalRecord(9, AsBytes("unknown"), false);

  const auto decoded = DecodeWalFragment(encoded);

  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().failure, WalDecodeFailure::ChecksumMismatch);
  EXPECT_EQ(decoded.error().recovery, WalRecoveryAction::DropBlock);
  EXPECT_EQ(decoded.error().encoded_size, 0U);
}

TEST(WalDecoderTest, CanDisableChecksumVerification) {
  std::vector<std::byte> encoded =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full), AsBytes("payload"), false);

  const auto verified = DecodeWalFragment(encoded);
  ASSERT_FALSE(verified.has_value());
  EXPECT_EQ(verified.error().failure, WalDecodeFailure::ChecksumMismatch);

  const auto unchecked = DecodeWalFragment(encoded, false);
  ASSERT_TRUE(unchecked.has_value());
  EXPECT_EQ(AsStringView(unchecked->payload), "payload");
}

TEST(WalDecoderTest, RejectsTruncatedHeaderAndPayload) {
  for (std::size_t size = 0; size < WalHeaderSize; ++size) {
    SCOPED_TRACE(size);
    const std::vector<std::byte> encoded(size, std::byte{0});
    const auto decoded = DecodeWalFragment(encoded);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().failure, WalDecodeFailure::TruncatedHeader);
    EXPECT_EQ(decoded.error().recovery, WalRecoveryAction::DropBlock);
  }

  std::vector<std::byte> encoded =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Full), AsBytes("abc"));
  encoded.pop_back();
  const auto decoded = DecodeWalFragment(encoded);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().failure, WalDecodeFailure::TruncatedPayload);
  EXPECT_EQ(decoded.error().recovery, WalRecoveryAction::DropBlock);
}

TEST(WalDecoderTest, RejectsPayloadLargerThanPhysicalBlockCapacity) {
  std::vector<std::byte> encoded(WalHeaderSize, std::byte{0});
  const std::size_t impossible_length = WalBlockSize - WalHeaderSize + 1U;
  encoded[4] = static_cast<std::byte>(impossible_length & 0xffU);
  encoded[5] = static_cast<std::byte>((impossible_length >> 8U) & 0xffU);
  encoded[6] = static_cast<std::byte>(WalRecordType::Full);

  const auto decoded = DecodeWalFragment(encoded);

  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().failure, WalDecodeFailure::PayloadTooLarge);
  EXPECT_EQ(decoded.error().recovery, WalRecoveryAction::DropBlock);
}

TEST(WalDecoderTest, TreatsNonemptyZeroTypeAsUnknown) {
  const std::vector<std::byte> encoded =
      PhysicalRecord(static_cast<std::uint8_t>(WalRecordType::Zero), AsBytes("x"));

  const auto decoded = DecodeWalFragment(encoded);

  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().failure, WalDecodeFailure::UnknownType);
  EXPECT_EQ(decoded.error().recovery, WalRecoveryAction::SkipPhysicalRecord);
}

}  // namespace
}  // namespace modern_leveldb
