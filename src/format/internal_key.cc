#include "format/internal_key.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

struct DecodedInternalKey {
  ByteView user_key;
  std::uint64_t trailer;
  SequenceNumber sequence;
  ValueKind kind;
};

constexpr bool IsValidValueKind(ValueKind kind) noexcept {
  return kind == ValueKind::Deletion || kind == ValueKind::Value;
}

constexpr std::uint64_t PackTrailer(SequenceNumber sequence, ValueKind kind) noexcept {
  return (sequence << 8U) | static_cast<std::uint8_t>(kind);
}

bool TryDecodeInternalKey(ByteView encoded, DecodedInternalKey& decoded) noexcept {
  if (encoded.size() < InternalKeyTrailerSize) {
    return false;
  }

  const std::size_t trailer_offset = encoded.size() - InternalKeyTrailerSize;
  std::uint64_t trailer = 0;
  for (std::size_t index = 0; index < InternalKeyTrailerSize; ++index) {
    trailer |= std::to_integer<std::uint64_t>(encoded[trailer_offset + index]) << (index * 8U);
  }

  const auto kind = static_cast<ValueKind>(trailer & 0xffU);
  if (!IsValidValueKind(kind)) {
    return false;
  }

  decoded = {
      .user_key = encoded.first(trailer_offset),
      .trailer = trailer,
      .sequence = trailer >> 8U,
      .kind = kind,
  };
  return true;
}

std::vector<std::byte> EncodeUnchecked(ByteView user_key, SequenceNumber sequence, ValueKind kind) {
  std::vector<std::byte> encoded;
  encoded.reserve(user_key.size() + InternalKeyTrailerSize);
  encoded.insert(encoded.end(), user_key.begin(), user_key.end());
  AppendFixed64(encoded, PackTrailer(sequence, kind));
  return encoded;
}

}  // namespace

Result<ParsedInternalKey> ParseInternalKey(ByteView encoded) {
  DecodedInternalKey decoded;
  if (!TryDecodeInternalKey(encoded, decoded)) {
    if (encoded.size() < InternalKeyTrailerSize) {
      return std::unexpected(Error::Corruption("internal key is shorter than its trailer"));
    }
    return std::unexpected(Error::Corruption("internal key has an unknown value kind"));
  }
  return ParsedInternalKey{
      .user_key = decoded.user_key,
      .sequence = decoded.sequence,
      .kind = decoded.kind,
  };
}

Result<InternalKey> InternalKey::Create(ByteView user_key, SequenceNumber sequence,
                                        ValueKind kind) {
  if (sequence > MaxSequenceNumber) {
    return std::unexpected(Error::InvalidArgument("internal key sequence exceeds 56 bits"));
  }
  if (!IsValidValueKind(kind)) {
    return std::unexpected(Error::InvalidArgument("internal key value kind is unsupported"));
  }
  return InternalKey(EncodeUnchecked(user_key, sequence, kind), sequence, kind);
}

Result<InternalKey> InternalKey::Decode(ByteView encoded) {
  const auto parsed = ParseInternalKey(encoded);
  if (!parsed.has_value()) {
    return std::unexpected(parsed.error());
  }
  return InternalKey(std::vector<std::byte>(encoded.begin(), encoded.end()), parsed->sequence,
                     parsed->kind);
}

ByteView InternalKey::user_key() const noexcept {
  if (encoded_.size() < InternalKeyTrailerSize) {
    return {};
  }
  return ByteView(encoded_).first(encoded_.size() - InternalKeyTrailerSize);
}

int InternalKeyComparator::Compare(ByteView left, ByteView right) const noexcept {
  DecodedInternalKey left_key;
  DecodedInternalKey right_key;
  const bool left_valid = TryDecodeInternalKey(left, left_key);
  const bool right_valid = TryDecodeInternalKey(right, right_key);

  if (left_valid != right_valid) {
    return left_valid ? 1 : -1;
  }
  if (!left_valid) {
    return BytewiseComparator().Compare(left, right);
  }

  const int user_order = user_comparator_.Compare(left_key.user_key, right_key.user_key);
  if (user_order != 0) {
    return user_order;
  }
  if (left_key.trailer > right_key.trailer) {
    return -1;
  }
  if (left_key.trailer < right_key.trailer) {
    return 1;
  }
  return 0;
}

std::string_view InternalKeyComparator::Name() const noexcept {
  return "leveldb.InternalKeyComparator";
}

void InternalKeyComparator::FindShortestSeparator(std::vector<std::byte>& start,
                                                  ByteView limit) const {
  DecodedInternalKey start_key;
  DecodedInternalKey limit_key;
  if (!TryDecodeInternalKey(start, start_key) || !TryDecodeInternalKey(limit, limit_key)) {
    return;
  }

  std::vector<std::byte> shortened(start_key.user_key.begin(), start_key.user_key.end());
  user_comparator_.FindShortestSeparator(shortened, limit_key.user_key);
  if (shortened.size() >= start_key.user_key.size() ||
      user_comparator_.Compare(start_key.user_key, shortened) >= 0) {
    return;
  }

  AppendFixed64(shortened, PackTrailer(MaxSequenceNumber, SeekValueKind));
  if (Compare(start, shortened) < 0 && Compare(shortened, limit) < 0) {
    start = std::move(shortened);
  }
}

void InternalKeyComparator::FindShortSuccessor(std::vector<std::byte>& key) const {
  DecodedInternalKey decoded;
  if (!TryDecodeInternalKey(key, decoded)) {
    return;
  }

  std::vector<std::byte> successor(decoded.user_key.begin(), decoded.user_key.end());
  user_comparator_.FindShortSuccessor(successor);
  if (successor.size() >= decoded.user_key.size() ||
      user_comparator_.Compare(decoded.user_key, successor) >= 0) {
    return;
  }

  AppendFixed64(successor, PackTrailer(MaxSequenceNumber, SeekValueKind));
  if (Compare(key, successor) < 0) {
    key = std::move(successor);
  }
}

}  // namespace modern_leveldb
