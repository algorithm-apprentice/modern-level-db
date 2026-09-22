#include "format/wal_format.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/crc32c.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t MaximumWalPayload = WalBlockSize - WalHeaderSize;

constexpr bool IsFragmentType(WalRecordType type) noexcept {
  return type == WalRecordType::Full || type == WalRecordType::First ||
         type == WalRecordType::Middle || type == WalRecordType::Last;
}

std::array<std::byte, WalHeaderSize> EncodeHeader(WalRecordType type, ByteView payload) noexcept {
  const std::byte type_byte = static_cast<std::byte>(type);
  std::uint32_t crc = ExtendCrc32c(Crc32c(ByteView(&type_byte, 1)), payload);
  crc = MaskCrc32c(crc);

  std::array<std::byte, WalHeaderSize> header{};
  for (std::size_t index = 0; index < sizeof(crc); ++index) {
    header[index] = static_cast<std::byte>((crc >> (index * 8U)) & 0xffU);
  }
  header[4] = static_cast<std::byte>(payload.size() & 0xffU);
  header[5] = static_cast<std::byte>((payload.size() >> 8U) & 0xffU);
  header[6] = type_byte;
  return header;
}

std::uint32_t DecodeStoredCrc(ByteView encoded) noexcept {
  std::uint32_t crc = 0;
  for (std::size_t index = 0; index < sizeof(crc); ++index) {
    crc |= std::to_integer<std::uint32_t>(encoded[index]) << (index * 8U);
  }
  return crc;
}

WalDecodeResult DecodeFailure(WalDecodeFailure failure, WalRecoveryAction recovery,
                              std::string message, std::size_t encoded_size = 0) {
  return std::unexpected(WalDecodeError{
      .error = Error::Corruption(std::move(message)),
      .failure = failure,
      .recovery = recovery,
      .encoded_size = encoded_size,
  });
}

}  // namespace

std::vector<WalFragment> WalFragmenter::Fragment(ByteView logical_record) {
  std::vector<WalFragment> fragments;
  std::size_t next_block_offset = block_offset_;
  ByteView remaining = logical_record;
  bool begin = true;

  do {
    const std::size_t bytes_left_in_block = WalBlockSize - next_block_offset;
    std::size_t padding_before = 0;
    if (bytes_left_in_block < WalHeaderSize) {
      padding_before = bytes_left_in_block;
      next_block_offset = 0;
    }

    const std::size_t available = WalBlockSize - next_block_offset - WalHeaderSize;
    const std::size_t fragment_size = std::min(remaining.size(), available);
    const bool end = fragment_size == remaining.size();

    WalRecordType type;
    if (begin && end) {
      type = WalRecordType::Full;
    } else if (begin) {
      type = WalRecordType::First;
    } else if (end) {
      type = WalRecordType::Last;
    } else {
      type = WalRecordType::Middle;
    }

    const ByteView payload = remaining.first(fragment_size);
    fragments.push_back(WalFragment{
        .padding_before = padding_before,
        .header = EncodeHeader(type, payload),
        .payload = payload,
    });

    next_block_offset += WalHeaderSize + fragment_size;
    remaining = remaining.subspan(fragment_size);
    begin = false;
  } while (!remaining.empty());

  block_offset_ = next_block_offset % WalBlockSize;
  return fragments;
}

WalDecodeResult DecodeWalFragment(ByteView encoded, bool verify_checksum) {
  if (encoded.size() < WalHeaderSize) {
    return DecodeFailure(WalDecodeFailure::TruncatedHeader, WalRecoveryAction::DropBlock,
                         "WAL physical record has a truncated header");
  }

  const std::size_t payload_size =
      std::to_integer<std::size_t>(encoded[4]) | (std::to_integer<std::size_t>(encoded[5]) << 8U);
  if (payload_size > MaximumWalPayload) {
    return DecodeFailure(WalDecodeFailure::PayloadTooLarge, WalRecoveryAction::DropBlock,
                         "WAL physical record payload exceeds block capacity");
  }

  const std::size_t encoded_size = WalHeaderSize + payload_size;
  if (encoded.size() < encoded_size) {
    return DecodeFailure(WalDecodeFailure::TruncatedPayload, WalRecoveryAction::DropBlock,
                         "WAL physical record has a truncated payload");
  }

  const auto type = static_cast<WalRecordType>(std::to_integer<std::uint8_t>(encoded[6]));
  if (type == WalRecordType::Zero && payload_size == 0) {
    return WalDecodeOutcome{
        .kind = WalDecodeKind::EndOfBlock,
        .type = WalRecordType::Zero,
        .payload = {},
        .encoded_size = WalHeaderSize,
    };
  }

  const ByteView payload = encoded.subspan(WalHeaderSize, payload_size);
  if (verify_checksum) {
    const std::uint32_t expected = UnmaskCrc32c(DecodeStoredCrc(encoded));
    const std::byte type_byte = encoded[6];
    const std::uint32_t actual = ExtendCrc32c(Crc32c(ByteView(&type_byte, 1)), payload);
    if (actual != expected) {
      return DecodeFailure(WalDecodeFailure::ChecksumMismatch, WalRecoveryAction::DropBlock,
                           "WAL physical record checksum mismatch");
    }
  }

  if (!IsFragmentType(type)) {
    return DecodeFailure(WalDecodeFailure::UnknownType, WalRecoveryAction::SkipPhysicalRecord,
                         "WAL physical record has an unknown type", encoded_size);
  }

  return WalDecodeOutcome{
      .kind = WalDecodeKind::Fragment,
      .type = type,
      .payload = payload,
      .encoded_size = encoded_size,
  };
}

}  // namespace modern_leveldb
