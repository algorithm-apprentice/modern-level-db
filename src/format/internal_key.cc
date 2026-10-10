#include "format/internal_key.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#if MODERN_LEVELDB_READ_DIAGNOSTICS
#include "instrumentation/read_diagnostics.h"
#endif
#include "base/coding_internal.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::uint64_t PackTrailer(SequenceNumber sequence, ValueKind kind) noexcept {
    return (sequence << 8U) | static_cast<std::uint8_t>(kind);
}

constexpr bool IsValidValueKind(ValueKind kind) noexcept {
    return kind == ValueKind::Deletion || kind == ValueKind::Value;
}

bool TryDecodeInternalKey(ByteView encoded, InternalKeyView& decoded) noexcept {
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

Result<InternalKeyView> ParseInternalKey(ByteView encoded) {
    InternalKeyView decoded;
    if (!TryDecodeInternalKey(encoded, decoded)) {
        if (encoded.size() < InternalKeyTrailerSize) {
            return std::unexpected(Error::Corruption("internal key is shorter than its trailer"));
        }
        return std::unexpected(Error::Corruption("internal key has an unknown value kind"));
    }
    return decoded;
}

Result<LookupKey> LookupKey::Create(ByteView user_key, SequenceNumber sequence) {
    if (sequence > MaxSequenceNumber) {
        return std::unexpected(Error::InvalidArgument("lookup key sequence exceeds 56 bits"));
    }
    return CreateTrusted(user_key, sequence);
}

LookupKey LookupKey::CreateTrusted(ByteView user_key, SequenceNumber sequence) {
    assert(sequence <= MaxSequenceNumber);
    const std::size_t internal_key_size = user_key.size() + InternalKeyTrailerSize;
    const auto encoded_internal_key_size = static_cast<std::uint32_t>(internal_key_size);
    const std::size_t prefix_size = VarintLength(encoded_internal_key_size);
    const std::size_t encoded_size = prefix_size + internal_key_size;

    LookupKey result;
    if (encoded_size > InlineCapacity) {
        result.heap_storage_ = std::make_unique<std::byte[]>(encoded_size);
    }
    result.encoded_size_ = encoded_size;
    result.internal_key_offset_ = prefix_size;

    MutableByteView output(result.data(), encoded_size);
    EncodeVarint32Trusted(output, encoded_internal_key_size);
    std::ranges::copy(user_key, output.begin());
    output = output.subspan(user_key.size());
    EncodeFixed64(
        std::span<std::byte, InternalKeyTrailerSize>(output.data(), InternalKeyTrailerSize),
        PackTrailer(sequence, SeekValueKind));
    return result;
}

LookupKey::LookupKey() noexcept { ResetToCanonicalEmpty(); }

LookupKey::LookupKey(LookupKey&& source) noexcept
    : inline_storage_(source.inline_storage_),
      heap_storage_(std::move(source.heap_storage_)),
      encoded_size_(source.encoded_size_),
      internal_key_offset_(source.internal_key_offset_) {
    source.ResetToCanonicalEmpty();
}

LookupKey& LookupKey::operator=(LookupKey&& source) noexcept {
    if (this != &source) {
        inline_storage_ = source.inline_storage_;
        heap_storage_ = std::move(source.heap_storage_);
        encoded_size_ = source.encoded_size_;
        internal_key_offset_ = source.internal_key_offset_;
        source.ResetToCanonicalEmpty();
    }
    return *this;
}

ByteView LookupKey::memtable_key() const noexcept { return ByteView(data(), encoded_size_); }

ByteView LookupKey::internal_key() const noexcept {
    return memtable_key().subspan(internal_key_offset_);
}

ByteView LookupKey::user_key() const noexcept {
    const ByteView encoded_internal_key = internal_key();
    assert(encoded_internal_key.size() >= InternalKeyTrailerSize);
    return encoded_internal_key.first(encoded_internal_key.size() - InternalKeyTrailerSize);
}

std::byte* LookupKey::data() noexcept {
    return heap_storage_ != nullptr ? heap_storage_.get() : inline_storage_.data();
}

const std::byte* LookupKey::data() const noexcept {
    return heap_storage_ != nullptr ? heap_storage_.get() : inline_storage_.data();
}

void LookupKey::ResetToCanonicalEmpty() noexcept {
    heap_storage_.reset();
    encoded_size_ = 1U + InternalKeyTrailerSize;
    internal_key_offset_ = 1;
    inline_storage_[0] = static_cast<std::byte>(InternalKeyTrailerSize);
    EncodeFixed64(std::span<std::byte, InternalKeyTrailerSize>(
                      inline_storage_.data() + internal_key_offset_, InternalKeyTrailerSize),
                  PackTrailer(0, SeekValueKind));
}

Result<InternalKey> InternalKey::Create(ByteView user_key, SequenceNumber sequence,
                                        ValueKind kind) {
    if (sequence > MaxSequenceNumber) {
        return std::unexpected(Error::InvalidArgument("internal key sequence exceeds 56 bits"));
    }
    if (!IsValidValueKind(kind)) {
        return std::unexpected(Error::InvalidArgument("internal key value kind is unsupported"));
    }
    return InternalKey(EncodeUnchecked(user_key, sequence, kind));
}

Result<InternalKey> InternalKey::Decode(ByteView encoded) {
    const auto parsed = ParseInternalKey(encoded);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    return InternalKey(std::vector<std::byte>(encoded.begin(), encoded.end()));
}

ByteView InternalKey::user_key() const noexcept {
    assert(encoded_.size() >= InternalKeyTrailerSize);
    return ByteView(encoded_).first(encoded_.size() - InternalKeyTrailerSize);
}

SequenceNumber InternalKey::sequence() const noexcept { return trailer() >> 8U; }

ValueKind InternalKey::kind() const noexcept { return static_cast<ValueKind>(trailer() & 0xffU); }

std::uint64_t InternalKey::trailer() const noexcept {
    assert(encoded_.size() >= InternalKeyTrailerSize);
    return DecodeFixed64(ByteView(encoded_).last<InternalKeyTrailerSize>());
}

int InternalKeyComparator::Compare(ByteView left, ByteView right) const noexcept {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
    read_diagnostics::Add(read_diagnostics::Counter::InternalKeyComparisons);
#endif
    InternalKeyView left_key;
    InternalKeyView right_key;
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
    const std::uint64_t left_trailer = PackTrailer(left_key.sequence, left_key.kind);
    const std::uint64_t right_trailer = PackTrailer(right_key.sequence, right_key.kind);
    if (left_trailer > right_trailer) {
        return -1;
    }
    if (left_trailer < right_trailer) {
        return 1;
    }
    return 0;
}

int InternalKeyComparator::CompareTrusted(ByteView left, ByteView right) const noexcept {
    assert(left.size() >= InternalKeyTrailerSize);
    assert(right.size() >= InternalKeyTrailerSize);
#if MODERN_LEVELDB_READ_DIAGNOSTICS
    read_diagnostics::Add(read_diagnostics::Counter::InternalKeyComparisons);
#endif
    const ByteView left_user_key = left.first(left.size() - InternalKeyTrailerSize);
    const ByteView right_user_key = right.first(right.size() - InternalKeyTrailerSize);
    const int user_order = user_comparator_.Compare(left_user_key, right_user_key);
    if (user_order != 0) {
        return user_order;
    }
    const std::uint64_t left_trailer = DecodeFixed64(left.last<InternalKeyTrailerSize>());
    const std::uint64_t right_trailer = DecodeFixed64(right.last<InternalKeyTrailerSize>());
    if (left_trailer > right_trailer) {
        return -1;
    }
    if (left_trailer < right_trailer) {
        return 1;
    }
    return 0;
}

std::string_view InternalKeyComparator::Name() const noexcept {
    return "leveldb.InternalKeyComparator";
}

void InternalKeyComparator::FindShortestSeparator(std::vector<std::byte>& start,
                                                  ByteView limit) const {
    InternalKeyView start_key;
    InternalKeyView limit_key;
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
    if (CompareTrusted(shortened, limit) < 0) {
        start = std::move(shortened);
    }
}

void InternalKeyComparator::FindShortSuccessor(std::vector<std::byte>& key) const {
    InternalKeyView decoded;
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
    key = std::move(successor);
}

}  // namespace modern_leveldb
