#include "format/write_batch.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t SequenceOffset = 0;
constexpr std::size_t CountOffset = sizeof(std::uint64_t);

bool IsSequenceRangeValid(SequenceNumber sequence, std::uint32_t count) noexcept {
  if (sequence > MaxSequenceNumber) {
    return false;
  }
  return count == 0 ||
         static_cast<SequenceNumber>(count - 1U) <= MaxSequenceNumber - sequence;
}

Result<ByteView> ConsumeBatchValue(ByteView& input) {
  Result<ByteView> value = ConsumeLengthPrefixed(input);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  return *value;
}

std::uint32_t ConsumeVarint32Unchecked(ByteView& input) noexcept {
  std::uint32_t value = 0;
  std::size_t shift = 0;
  while (true) {
    const unsigned int byte = std::to_integer<unsigned int>(input.front());
    input = input.subspan(1);
    value |= static_cast<std::uint32_t>(byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0U) {
      return value;
    }
    shift += 7U;
  }
}

ByteView ConsumeLengthPrefixedUnchecked(ByteView& input) noexcept {
  const std::uint32_t length = ConsumeVarint32Unchecked(input);
  const ByteView value = input.first(length);
  input = input.subspan(length);
  return value;
}

Status AppendRecord(std::vector<std::byte>& destination, ValueKind kind, ByteView key,
                    ByteView value) {
  std::vector<std::byte> record;
  record.push_back(static_cast<std::byte>(kind));

  Status key_status = AppendLengthPrefixed(record, key);
  if (!key_status.has_value()) {
    return std::unexpected(key_status.error());
  }
  if (kind == ValueKind::Value) {
    Status value_status = AppendLengthPrefixed(record, value);
    if (!value_status.has_value()) {
      return std::unexpected(value_status.error());
    }
  }

  destination.insert(destination.end(), record.begin(), record.end());
  return {};
}

}  // namespace

Result<WriteBatchReader> WriteBatchReader::Open(ByteView encoded) {
  if (encoded.size() < WriteBatchHeaderSize) {
    return std::unexpected(Error::Corruption("write batch is shorter than its header"));
  }

  const SequenceNumber sequence =
      DecodeFixed64(encoded.subspan<SequenceOffset, sizeof(SequenceNumber)>());
  const std::uint32_t count =
      DecodeFixed32(encoded.subspan<CountOffset, sizeof(std::uint32_t)>());
  if (!IsSequenceRangeValid(sequence, count)) {
    return std::unexpected(Error::Corruption("write batch sequence range exceeds 56 bits"));
  }

  ByteView remaining = encoded.subspan(WriteBatchHeaderSize);
  std::uint32_t records_left = count;
  while (records_left > 0) {
    if (remaining.empty()) {
      return std::unexpected(Error::Corruption("write batch count exceeds encoded records"));
    }

    const auto kind = static_cast<ValueKind>(std::to_integer<std::uint8_t>(remaining.front()));
    remaining = remaining.subspan(1);
    if (kind != ValueKind::Deletion && kind != ValueKind::Value) {
      return std::unexpected(Error::Corruption("write batch has an unknown value kind"));
    }

    Result<ByteView> key = ConsumeBatchValue(remaining);
    if (!key.has_value()) {
      return std::unexpected(key.error());
    }
    if (kind == ValueKind::Value) {
      Result<ByteView> value = ConsumeBatchValue(remaining);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
    }
    --records_left;
  }

  if (!remaining.empty()) {
    return std::unexpected(Error::Corruption("write batch has trailing bytes"));
  }

  return WriteBatchReader(encoded.subspan(WriteBatchHeaderSize), sequence, count);
}

std::optional<WriteBatchEntry> WriteBatchReader::Next() noexcept {
  if (index_ == count_) {
    return std::nullopt;
  }

  const auto kind = static_cast<ValueKind>(std::to_integer<std::uint8_t>(remaining_.front()));
  remaining_ = remaining_.subspan(1);
  const ByteView key = ConsumeLengthPrefixedUnchecked(remaining_);
  ByteView value;
  if (kind == ValueKind::Value) {
    value = ConsumeLengthPrefixedUnchecked(remaining_);
  }

  const WriteBatchEntry entry{
      .sequence = sequence_ + index_,
      .kind = kind,
      .key = key,
      .value = value,
  };
  ++index_;
  return entry;
}

WriteBatch::WriteBatch() : encoded_(WriteBatchHeaderSize) {}

WriteBatch::WriteBatch(WriteBatch&& source) : encoded_(WriteBatchHeaderSize) {
  encoded_.swap(source.encoded_);
}

WriteBatch& WriteBatch::operator=(WriteBatch&& source) {
  if (this != &source) {
    WriteBatch replacement(std::move(source));
    encoded_.swap(replacement.encoded_);
  }
  return *this;
}

Status WriteBatch::Put(ByteView key, ByteView value) {
  Status validation = ValidateAdditionalRecords(1);
  if (!validation.has_value()) {
    return validation;
  }

  Status appended = AppendRecord(encoded_, ValueKind::Value, key, value);
  if (!appended.has_value()) {
    return appended;
  }
  SetCount(count() + 1U);
  return {};
}

Status WriteBatch::Delete(ByteView key) {
  Status validation = ValidateAdditionalRecords(1);
  if (!validation.has_value()) {
    return validation;
  }

  Status appended = AppendRecord(encoded_, ValueKind::Deletion, key, {});
  if (!appended.has_value()) {
    return appended;
  }
  SetCount(count() + 1U);
  return {};
}

Status WriteBatch::Append(const WriteBatch& source) {
  const std::uint32_t source_count = source.count();
  if (source_count == 0) {
    return {};
  }

  Status validation = ValidateAdditionalRecords(source_count);
  if (!validation.has_value()) {
    return validation;
  }

  const ByteView source_records = source.encoded().subspan(WriteBatchHeaderSize);
  if (this == &source) {
    const std::vector<std::byte> stable_records(source_records.begin(), source_records.end());
    encoded_.insert(encoded_.end(), stable_records.begin(), stable_records.end());
  } else {
    encoded_.insert(encoded_.end(), source_records.begin(), source_records.end());
  }
  SetCount(count() + source_count);
  return {};
}

Status WriteBatch::SetSequence(SequenceNumber sequence) {
  if (!IsSequenceRangeValid(sequence, count())) {
    return std::unexpected(Error::InvalidArgument("write batch sequence range exceeds 56 bits"));
  }
  EncodeFixed64(
      MutableByteView(encoded_).subspan<SequenceOffset, sizeof(SequenceNumber)>(), sequence);
  return {};
}

void WriteBatch::Clear() noexcept {
  encoded_.resize(WriteBatchHeaderSize);
  std::ranges::fill(encoded_, std::byte{0});
}

SequenceNumber WriteBatch::sequence() const noexcept {
  return DecodeFixed64(ByteView(encoded_).subspan<SequenceOffset, sizeof(SequenceNumber)>());
}

std::uint32_t WriteBatch::count() const noexcept {
  return DecodeFixed32(ByteView(encoded_).subspan<CountOffset, sizeof(std::uint32_t)>());
}

Status WriteBatch::ValidateAdditionalRecords(std::uint32_t additional) const {
  const std::uint32_t current_count = count();
  if (additional > std::numeric_limits<std::uint32_t>::max() - current_count) {
    return std::unexpected(Error::InvalidArgument("write batch record count exceeds uint32"));
  }

  const std::uint32_t new_count = current_count + additional;
  if (!IsSequenceRangeValid(sequence(), new_count)) {
    return std::unexpected(Error::InvalidArgument("write batch sequence range exceeds 56 bits"));
  }
  return {};
}

void WriteBatch::SetCount(std::uint32_t count) noexcept {
  EncodeFixed32(
      MutableByteView(encoded_).subspan<CountOffset, sizeof(std::uint32_t)>(), count);
}

}  // namespace modern_leveldb
