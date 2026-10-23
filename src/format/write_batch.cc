#include "format/write_batch.h"

#include <algorithm>
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

namespace {

Status ValidateBatchRecord(ByteView& remaining) {
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
    return {};
}

Status ValidateBatchRecords(ByteView remaining, std::uint32_t count) {
    for (std::uint32_t index = 0; index < count; ++index) {
        Status record = ValidateBatchRecord(remaining);
        if (!record.has_value()) {
            return std::unexpected(std::move(record).error());
        }
    }

    if (!remaining.empty()) {
        return std::unexpected(Error::Corruption("write batch has trailing bytes"));
    }

    return {};
}

std::size_t EncodedRecordSize(ValueKind kind, ByteView key, ByteView value) noexcept {
    const auto key_length = static_cast<std::uint32_t>(key.size());
    const auto value_length = static_cast<std::uint32_t>(value.size());
    return 1 + VarintLengthTrusted(key_length) + key.size() +
           (kind == ValueKind::Value ? VarintLengthTrusted(value_length) + value.size() : 0);
}

void EncodeTrustedBatchRecord(MutableByteView& output, ValueKind kind, ByteView key,
                              ByteView value) noexcept {
    output.front() = static_cast<std::byte>(kind);
    output = output.subspan(1);
    EncodeLengthPrefixedTrusted(output, key);
    if (kind == ValueKind::Value) {
        EncodeLengthPrefixedTrusted(output, value);
    }
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

    // Validate everything once so iteration can decode borrowed records without
    // repeating recoverable checks. Mutating the backing bytes breaks this proof.
    Status records = ValidateBatchRecords(encoded.subspan(WriteBatchHeaderSize), count);
    if (!records.has_value()) {
        return std::unexpected(std::move(records).error());
    }
    return WriteBatchReader(encoded.subspan(WriteBatchHeaderSize), sequence, count);
}

WriteBatchReader WriteBatchReader::OpenTrusted(const EncodedWriteBatch& batch) noexcept {
    return WriteBatchReader(batch.encoded().subspan(WriteBatchHeaderSize), batch.sequence(),
                            batch.count());
}

std::optional<WriteBatchEntry> WriteBatchReader::Next() noexcept {
    if (index_ == count_) {
        return std::nullopt;
    }

    return ConsumeEntry();
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
}

void EncodedWriteBatch::Delete(ByteView key) { AppendRecord(ValueKind::Deletion, key, {}); }

void EncodedWriteBatch::Append(const EncodedWriteBatch& source) {
    const std::uint32_t source_count = source.count();
    if (source_count == 0) {
        return;
    }

    encoded_.append(source.encoded_, WriteBatchHeaderSize, std::string::npos);
    SetCount(count() + source_count);
}

void EncodedWriteBatch::AppendRecord(ValueKind kind, ByteView key, ByteView value) {
    const std::size_t record_size = EncodedRecordSize(kind, key, value);
    const std::size_t old_size = encoded_.size();
    encoded_.resize_and_overwrite(old_size + record_size, [=](char* data, std::size_t) noexcept {
        MutableByteView output = AsWritableBytes(std::span<char>(data + old_size, record_size));
        EncodeTrustedBatchRecord(output, kind, key, value);
        return old_size + record_size;
    });
    SetCount(count() + 1U);
}

void EncodedWriteBatch::Clear() noexcept {
    encoded_.resize(WriteBatchHeaderSize);
    std::ranges::fill(encoded_, '\0');
}

}  // namespace modern_leveldb
