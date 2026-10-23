#include "format/internal_key.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "base/coding_internal.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

enum class InternalKeyDecodeIssue {
    None,
    ShortTrailer,
    UnknownKind,
};

InternalKeyDecodeIssue DecodeInternalKeyView(ByteView encoded, InternalKeyView& decoded) noexcept {
    if (encoded.size() < InternalKeyTrailerSize) {
        return InternalKeyDecodeIssue::ShortTrailer;
    }

    const std::size_t trailer_offset = encoded.size() - InternalKeyTrailerSize;
    const std::uint64_t trailer = DecodeInternalKeyTrailer(encoded);
    const ValueKind kind = DecodeInternalKeyValueKind(trailer);
    if (!IsValidValueKind(kind)) {
        return InternalKeyDecodeIssue::UnknownKind;
    }

    const InternalKeyView complete{
        .user_key = encoded.first(trailer_offset),
        .sequence = DecodeInternalKeySequence(trailer),
        .kind = kind,
    };
    decoded = complete;
    return InternalKeyDecodeIssue::None;
}

bool TryDecodeInternalKey(ByteView encoded, InternalKeyView& decoded) noexcept {
    return DecodeInternalKeyView(encoded, decoded) == InternalKeyDecodeIssue::None;
}

Status ValidateInternalKeyInput(SequenceNumber sequence, ValueKind kind) {
    if (sequence > MaxSequenceNumber) {
        return std::unexpected(Error::InvalidArgument("internal key sequence exceeds 56 bits"));
    }
    if (!IsValidValueKind(kind)) {
        return std::unexpected(Error::InvalidArgument("internal key value kind is unsupported"));
    }
    return {};
}

std::vector<std::byte> EncodeUnchecked(ByteView user_key, SequenceNumber sequence, ValueKind kind) {
    std::vector<std::byte> encoded;
    encoded.reserve(user_key.size() + InternalKeyTrailerSize);
    encoded.insert(encoded.end(), user_key.begin(), user_key.end());
    AppendFixed64(encoded, PackInternalKeyTrailer(sequence, kind));
    return encoded;
}

int CompareDescendingTrailers(ByteView left, ByteView right) noexcept {
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

bool IsShorterAndGreaterUserKey(const Comparator& comparator, ByteView original,
                                ByteView candidate) noexcept {
    if (candidate.size() >= original.size()) {
        return false;
    }
    return comparator.Compare(original, candidate) < 0;
}

void AppendMaxSeekTrailer(std::vector<std::byte>& key) {
    AppendFixed64(key, PackInternalKeyTrailer(MaxSequenceNumber, SeekValueKind));
}

void CommitSeparatorBeforeLimit(const InternalKeyComparator& comparator,
                                std::vector<std::byte>& start, std::vector<std::byte>& candidate,
                                ByteView limit) {
    if (comparator.Compare(candidate, limit) < 0) {
        start = std::move(candidate);
    }
}

}  // namespace

void EncodeInternalKeyTrusted(MutableByteView& output, ByteView user_key, SequenceNumber sequence,
                              ValueKind kind) noexcept {
    std::ranges::copy(user_key, output.begin());
    output = output.subspan(user_key.size());
    EncodeFixed64(
        std::span<std::byte, InternalKeyTrailerSize>(output.data(), InternalKeyTrailerSize),
        PackInternalKeyTrailer(sequence, kind));
    output = output.subspan(InternalKeyTrailerSize);
}

Result<InternalKeyView> ParseInternalKey(ByteView encoded) {
    InternalKeyView decoded;
    const InternalKeyDecodeIssue issue = DecodeInternalKeyView(encoded, decoded);
    if (issue == InternalKeyDecodeIssue::ShortTrailer) {
        return std::unexpected(Error::Corruption("internal key is shorter than its trailer"));
    }
    if (issue == InternalKeyDecodeIssue::UnknownKind) {
        return std::unexpected(Error::Corruption("internal key has an unknown value kind"));
    }
    return decoded;
}

Result<LookupKey> LookupKey::Create(const ByteView& user_key, SequenceNumber sequence) {
    if (sequence > MaxSequenceNumber) {
        return std::unexpected(Error::InvalidArgument("lookup key sequence exceeds 56 bits"));
    }
    return CreateTrusted(user_key, sequence);
}

LookupKey LookupKey::CreateTrusted(const ByteView& user_key, SequenceNumber sequence) {
    const std::size_t internal_key_size = user_key.size() + InternalKeyTrailerSize;
    const auto encoded_internal_key_size = static_cast<std::uint32_t>(internal_key_size);
    const std::size_t prefix_size = VarintLength(encoded_internal_key_size);
    const std::size_t encoded_size = prefix_size + internal_key_size;

    LookupKey result(encoded_size, prefix_size);

    MutableByteView output(result.data(), encoded_size);
    EncodeVarint32Trusted(output, encoded_internal_key_size);
    EncodeInternalKeyTrusted(output, user_key, sequence, SeekValueKind);
    return result;
}

LookupKey::LookupKey(std::size_t encoded_size, std::size_t prefix_size)
    : encoded_size_(encoded_size), internal_key_offset_(prefix_size) {
    if (encoded_size > InlineCapacity) {
        heap_storage_ = std::make_unique_for_overwrite<std::byte[]>(encoded_size);
    }
}

LookupKey::LookupKey(LookupKey&& source) noexcept
    : inline_storage_(source.inline_storage_),
      heap_storage_(std::move(source.heap_storage_)),
      encoded_size_(source.encoded_size_),
      internal_key_offset_(source.internal_key_offset_) {
    source.ResetToCanonicalEmpty();
}

LookupKey& LookupKey::operator=(LookupKey&& source) noexcept {
    if (this != &source) {
        TransferStorageFrom(source);
        source.ResetToCanonicalEmpty();
    }
    return *this;
}

void LookupKey::TransferStorageFrom(LookupKey& source) noexcept {
    inline_storage_ = source.inline_storage_;
    heap_storage_ = std::move(source.heap_storage_);
    encoded_size_ = source.encoded_size_;
    internal_key_offset_ = source.internal_key_offset_;
}

ByteView LookupKey::memtable_key() const noexcept { return ByteView(data(), encoded_size_); }

ByteView LookupKey::internal_key() const noexcept {
    return memtable_key().subspan(internal_key_offset_);
}

ByteView LookupKey::user_key() const noexcept {
    const ByteView encoded_internal_key = internal_key();
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
    InitializeCanonicalEmpty();
}

void LookupKey::InitializeCanonicalEmpty() noexcept {
    encoded_size_ = 1U + InternalKeyTrailerSize;
    internal_key_offset_ = 1;
    inline_storage_[0] = static_cast<std::byte>(InternalKeyTrailerSize);
    EncodeFixed64(std::span<std::byte, InternalKeyTrailerSize>(
                      inline_storage_.data() + internal_key_offset_, InternalKeyTrailerSize),
                  PackInternalKeyTrailer(0, SeekValueKind));
}

Result<InternalKey> InternalKey::Create(ByteView user_key, SequenceNumber sequence,
                                        ValueKind kind) {
    Status valid = ValidateInternalKeyInput(sequence, kind);
    if (!valid.has_value()) {
        return std::unexpected(std::move(valid).error());
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
    return ByteView(encoded_).first(encoded_.size() - InternalKeyTrailerSize);
}

SequenceNumber InternalKey::sequence() const noexcept {
    return DecodeInternalKeySequence(trailer());
}

ValueKind InternalKey::kind() const noexcept { return DecodeInternalKeyValueKind(trailer()); }

std::uint64_t InternalKey::trailer() const noexcept { return DecodeInternalKeyTrailer(encoded_); }

int InternalKeyComparator::Compare(const ByteView& left, const ByteView& right) const noexcept {
    const ByteView left_user_key = left.first(left.size() - InternalKeyTrailerSize);
    const ByteView right_user_key = right.first(right.size() - InternalKeyTrailerSize);
    const int user_order = user_comparator_.Compare(left_user_key, right_user_key);
    if (user_order != 0) {
        return user_order;
    }
    return CompareDescendingTrailers(left, right);
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
    if (!IsShorterAndGreaterUserKey(user_comparator_, start_key.user_key, shortened)) {
        return;
    }

    AppendMaxSeekTrailer(shortened);
    CommitSeparatorBeforeLimit(*this, start, shortened, limit);
}

void InternalKeyComparator::FindShortSuccessor(std::vector<std::byte>& key) const {
    InternalKeyView decoded;
    if (!TryDecodeInternalKey(key, decoded)) {
        return;
    }

    std::vector<std::byte> successor(decoded.user_key.begin(), decoded.user_key.end());
    user_comparator_.FindShortSuccessor(successor);
    if (!IsShorterAndGreaterUserKey(user_comparator_, decoded.user_key, successor)) {
        return;
    }

    AppendMaxSeekTrailer(successor);
    key = std::move(successor);
}

}  // namespace modern_leveldb
