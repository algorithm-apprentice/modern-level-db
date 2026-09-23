#include "memory/memtable.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

struct EntryView {
  ByteView internal_key;
  ByteView value;
};

bool IsValidValueKind(ValueKind kind) noexcept {
  return kind == ValueKind::Deletion || kind == ValueKind::Value;
}

std::uint64_t PackTrailer(SequenceNumber sequence, ValueKind kind) noexcept {
  return (sequence << 8U) | static_cast<std::uint8_t>(kind);
}

std::uint32_t DecodeVarint32Unchecked(const std::byte*& input) noexcept {
  std::uint32_t value = 0;
  std::size_t shift = 0;
  while (true) {
    const unsigned int byte = std::to_integer<unsigned int>(*input);
    ++input;
    value |= static_cast<std::uint32_t>(byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0U) {
      return value;
    }
    shift += 7U;
  }
}

ByteView DecodeLengthPrefixedUnchecked(const std::byte*& input) noexcept {
  const std::uint32_t length = DecodeVarint32Unchecked(input);
  const ByteView value(input, length);
  input += length;
  return value;
}

ByteView DecodeInternalKey(const std::byte* entry) noexcept {
  return DecodeLengthPrefixedUnchecked(entry);
}

EntryView DecodeEntry(const std::byte* entry) noexcept {
  const ByteView internal_key = DecodeLengthPrefixedUnchecked(entry);
  const ByteView value = DecodeLengthPrefixedUnchecked(entry);
  return {
      .internal_key = internal_key,
      .value = value,
  };
}

ValueKind DecodeValueKind(ByteView internal_key) noexcept {
  const ByteView trailer =
      internal_key.last<InternalKeyTrailerSize>();
  return static_cast<ValueKind>(DecodeFixed64(
                                    std::span<const std::byte, InternalKeyTrailerSize>(
                                        trailer.data(), InternalKeyTrailerSize)) &
                                0xffU);
}

bool TryAddSize(std::size_t& total, std::size_t value) noexcept {
  if (value > std::numeric_limits<std::size_t>::max() - total) {
    return false;
  }
  total += value;
  return true;
}

}  // namespace

MemTable::MemTable(const Comparator& user_comparator)
    : user_comparator_(user_comparator),
      internal_comparator_(user_comparator),
      entry_comparator_{internal_comparator_},
      table_(entry_comparator_, arena_) {}

Status MemTable::Add(SequenceNumber sequence, ValueKind kind, ByteView key,
                     ByteView value) {
  if (sequence > MaxSequenceNumber) {
    return std::unexpected(Error::InvalidArgument("memtable sequence exceeds 56 bits"));
  }
  if (!IsValidValueKind(kind)) {
    return std::unexpected(Error::InvalidArgument("memtable value kind is unsupported"));
  }

  constexpr std::size_t MaximumLength =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
  if (key.size() > MaximumLength - InternalKeyTrailerSize) {
    return std::unexpected(Error::InvalidArgument("memtable internal key exceeds uint32"));
  }
  if (value.size() > MaximumLength) {
    return std::unexpected(Error::InvalidArgument("memtable value exceeds uint32"));
  }

  const std::size_t internal_key_size = key.size() + InternalKeyTrailerSize;
  std::size_t encoded_size = VarintLength(internal_key_size);
  if (!TryAddSize(encoded_size, internal_key_size) ||
      !TryAddSize(encoded_size, VarintLength(value.size())) ||
      !TryAddSize(encoded_size, value.size())) {
    return std::unexpected(Error::InvalidArgument("memtable entry is too large"));
  }

  MutableByteView output = arena_.Allocate(encoded_size);
  const std::byte* const entry = output.data();
  const bool encoded_key_length =
      EncodeVarint32(output, static_cast<std::uint32_t>(internal_key_size));
  assert(encoded_key_length);
  (void)encoded_key_length;

  std::ranges::copy(key, output.begin());
  output = output.subspan(key.size());
  EncodeFixed64(std::span<std::byte, InternalKeyTrailerSize>(
                    output.data(), InternalKeyTrailerSize),
                PackTrailer(sequence, kind));
  output = output.subspan(InternalKeyTrailerSize);

  const bool encoded_value_length =
      EncodeVarint32(output, static_cast<std::uint32_t>(value.size()));
  assert(encoded_value_length);
  (void)encoded_value_length;
  std::ranges::copy(value, output.begin());
  output = output.subspan(value.size());
  assert(output.empty());

  if (!table_.Insert(entry)) {
    return std::unexpected(Error::InvalidArgument("memtable internal key already exists"));
  }
  return {};
}

MemTableLookup MemTable::Lookup(const LookupKey& key) const {
  Table::Iterator iterator(table_);
  iterator.Seek(key.memtable_key().data());
  if (!iterator.valid()) {
    return {};
  }

  const EntryView entry = DecodeEntry(iterator.key());
  const ByteView candidate_user_key =
      entry.internal_key.first(entry.internal_key.size() - InternalKeyTrailerSize);
  if (user_comparator_.Compare(candidate_user_key, key.user_key()) != 0) {
    return {};
  }

  if (DecodeValueKind(entry.internal_key) == ValueKind::Deletion) {
    return {
        .kind = MemTableLookupKind::Deletion,
        .value = {},
    };
  }
  return {
      .kind = MemTableLookupKind::Value,
      .value = entry.value,
  };
}

int MemTable::EntryComparator::operator()(const std::byte* left,
                                          const std::byte* right) const noexcept {
  return comparator.Compare(DecodeInternalKey(left), DecodeInternalKey(right));
}

MemTable::Iterator::Iterator(const MemTable& table) noexcept : iterator_(table.table_) {}

ByteView MemTable::Iterator::key() const {
  return DecodeEntry(iterator_.key()).internal_key;
}

ByteView MemTable::Iterator::value() const {
  return DecodeEntry(iterator_.key()).value;
}

Status MemTable::Iterator::Seek(ByteView internal_key) {
  constexpr std::size_t MaximumLength =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
  if (internal_key.size() > MaximumLength) {
    return std::unexpected(Error::InvalidArgument("memtable seek key exceeds uint32"));
  }

  const std::size_t prefix_size = VarintLength(internal_key.size());
  if (internal_key.size() > std::numeric_limits<std::size_t>::max() - prefix_size) {
    return std::unexpected(Error::InvalidArgument("memtable seek representation is too large"));
  }

  seek_key_.clear();
  Status encoded = AppendLengthPrefixed(seek_key_, internal_key);
  if (!encoded.has_value()) {
    return encoded;
  }
  iterator_.Seek(seek_key_.data());
  return {};
}

}  // namespace modern_leveldb
