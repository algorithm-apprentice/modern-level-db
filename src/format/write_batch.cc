#include "format/write_batch.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "base/coding_internal.h"
#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
constexpr std::size_t SequenceOffset = 0;
constexpr std::size_t CountOffset = sizeof(std::uint64_t);

Result<WriteBatchReader> WriteBatchReader::Open(ByteView encoded) {
    if (encoded.size() < WriteBatchHeaderSize) {
        return std::unexpected(Error::Corruption("write batch is shorter than its header"));
    }

    const SequenceNumber sequence =
        DecodeFixed64(encoded.subspan<SequenceOffset, sizeof(SequenceNumber)>());
    const std::uint32_t count =
        DecodeFixed32(encoded.subspan<CountOffset, sizeof(std::uint32_t)>());

    // Validate everything once so Next can decode borrowed records without
    // repeating recoverable checks. Mutating the backing bytes breaks this proof.
    ByteView remaining = encoded.subspan(WriteBatchHeaderSize);
    std::uint32_t records_left = count;
    while (records_left > 0) {
        if (remaining.empty()) {
            return std::unexpected(Error::Corruption("write batch count exceeds encoded records"));
        }

        const ValueKind kind = ConsumeValueKindTrusted(remaining);
        if (!IsValidValueKind(kind)) {
            return std::unexpected(Error::Corruption("write batch has an unknown value kind"));
        }

        Result<ByteView> key = ConsumeLengthPrefixed(remaining);
        if (!key.has_value()) {
            return std::unexpected(key.error());
        }
        if (kind == ValueKind::Value) {
            Result<ByteView> value = ConsumeLengthPrefixed(remaining);
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

WriteBatchReader WriteBatchReader::OpenTrusted(const EncodedWriteBatch& batch) noexcept {
    assert(batch.encoded().size() >= WriteBatchHeaderSize);
    return WriteBatchReader(batch.encoded().subspan(WriteBatchHeaderSize), batch.sequence(),
                            batch.count());
}

std::optional<WriteBatchEntry> WriteBatchReader::Next() noexcept {
    if (index_ == count_) {
        return std::nullopt;
    }

    const ValueKind kind = ConsumeValueKindTrusted(remaining_);
    const ByteView key = ConsumeLengthPrefixedTrusted(remaining_);
    ByteView value;
    if (kind == ValueKind::Value) {
        value = ConsumeLengthPrefixedTrusted(remaining_);
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

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
EncodedWriteBatch::EncodedWriteBatch() : encoded_(WriteBatchHeaderSize, '\0') {}

EncodedWriteBatch::EncodedWriteBatch(EncodedWriteBatch&& source)
    : encoded_(WriteBatchHeaderSize, '\0') {
    encoded_.swap(source.encoded_);
}
// GCOVR_EXCL_STOP

EncodedWriteBatch& EncodedWriteBatch::operator=(EncodedWriteBatch&& source) {
    if (this != &source) {
        EncodedWriteBatch replacement(std::move(source));
        encoded_.swap(replacement.encoded_);
    }
    return *this;
}

void EncodedWriteBatch::Put(ByteView key, ByteView value) {
    AppendRecord(ValueKind::Value, key, value);
    SetCount(count() + 1U);
}

void EncodedWriteBatch::Delete(ByteView key) {
    AppendRecord(ValueKind::Deletion, key, {});
    SetCount(count() + 1U);
}

void EncodedWriteBatch::Append(const EncodedWriteBatch& source) {
    const std::uint32_t source_count = source.count();
    if (source_count == 0) {
        return;
    }

    encoded_.append(source.encoded_, WriteBatchHeaderSize, std::string::npos);
    SetCount(count() + source_count);
}

void EncodedWriteBatch::AppendRecord(ValueKind kind, ByteView key, ByteView value) {
    const auto key_length = static_cast<std::uint32_t>(key.size());
    const auto value_length = static_cast<std::uint32_t>(value.size());

    const std::size_t record_size =
        1 + VarintLength(key_length) + key.size() +
        (kind == ValueKind::Value ? VarintLength(value_length) + value.size() : 0);

    const std::size_t old_size = encoded_.size();
    encoded_.resize(old_size + record_size);
    MutableByteView output =
        AsWritableBytes(std::span<char>(encoded_.data() + old_size, encoded_.size() - old_size));
    output.front() = static_cast<std::byte>(kind);
    output = output.subspan(1);
    EncodeLengthPrefixedTrusted(output, key);
    if (kind == ValueKind::Value) {
        EncodeLengthPrefixedTrusted(output, value);
    }
    assert(output.empty());
}

void EncodedWriteBatch::SetSequence(SequenceNumber sequence) noexcept {
    MutableByteView bytes = AsWritableBytes(std::span<char>(encoded_.data(), encoded_.size()));
    EncodeFixed64(bytes.subspan<SequenceOffset, sizeof(SequenceNumber)>(), sequence);
}

void EncodedWriteBatch::Clear() noexcept {
    encoded_.resize(WriteBatchHeaderSize);
    std::ranges::fill(encoded_, '\0');
}

SequenceNumber EncodedWriteBatch::sequence() const noexcept {
    return DecodeFixed64(encoded().subspan<SequenceOffset, sizeof(SequenceNumber)>());
}

std::uint32_t EncodedWriteBatch::count() const noexcept {
    return DecodeFixed32(encoded().subspan<CountOffset, sizeof(std::uint32_t)>());
}

void EncodedWriteBatch::SetCount(std::uint32_t count) noexcept {
    MutableByteView bytes = AsWritableBytes(std::span<char>(encoded_.data(), encoded_.size()));
    EncodeFixed32(bytes.subspan<CountOffset, sizeof(std::uint32_t)>(), count);
}

}  // namespace modern_leveldb
