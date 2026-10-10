#include "format/write_batch.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/coding_internal.h"
#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t SequenceOffset = 0;
constexpr std::size_t CountOffset = sizeof(std::uint64_t);
constexpr std::size_t MaximumKeySize =
    std::numeric_limits<std::uint32_t>::max() - InternalKeyTrailerSize;

bool IsSequenceRangeValid(SequenceNumber sequence, std::uint32_t count) noexcept {
    if (sequence > MaxSequenceNumber) {
        return false;
    }
    return count == 0 || static_cast<SequenceNumber>(count - 1U) <= MaxSequenceNumber - sequence;
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

// GCOVR_EXCL_START: these require one logical value larger than 4 GiB
std::unexpected<Error> KeyTooLong() {
    return std::unexpected(Error::InvalidArgument("write batch key is too long for the memtable"));
}

std::unexpected<Error> ValueTooLong() {
    return std::unexpected(Error::InvalidArgument("write batch value exceeds uint32"));
}
// GCOVR_EXCL_STOP

// GCOVR_EXCL_START: representable records exceed supported string storage
std::unexpected<Error> BatchTooLarge() {
    return std::unexpected(Error::InvalidArgument("write batch representation is too large"));
}
// GCOVR_EXCL_STOP

void EncodeBatchValue(MutableByteView& output, ByteView value) noexcept {
    EncodeVarint32Trusted(output, static_cast<std::uint32_t>(value.size()));
    std::ranges::copy(value, output.begin());
    output = output.subspan(value.size());
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

    // Validate everything once so Next can decode borrowed records without
    // repeating recoverable checks. Mutating the backing bytes breaks this proof.
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

WriteBatchReader WriteBatchReader::OpenTrusted(const EncodedWriteBatch& batch) noexcept {
    assert(batch.encoded().size() >= WriteBatchHeaderSize);
    assert(IsSequenceRangeValid(batch.sequence(), batch.count()));
    return WriteBatchReader(batch.encoded().subspan(WriteBatchHeaderSize), batch.sequence(),
                            batch.count());
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

Status EncodedWriteBatch::Put(ByteView key, ByteView value) {
    const Status validation = ValidateAdditionalRecords(1);
    if (!validation.has_value()) {
        return validation;
    }
    const Status appended = AppendRecord(ValueKind::Value, key, value);
    if (!appended.has_value()) {
        return appended;
    }
    SetCount(count() + 1U);
    return {};
}

Status EncodedWriteBatch::Delete(ByteView key) {
    const Status validation = ValidateAdditionalRecords(1);
    if (!validation.has_value()) {
        return validation;
    }
    const Status appended = AppendRecord(ValueKind::Deletion, key, {});
    if (!appended.has_value()) {
        return appended;
    }
    SetCount(count() + 1U);
    return {};
}

Status EncodedWriteBatch::Append(const EncodedWriteBatch& source) {
    const std::uint32_t source_count = source.count();
    if (source_count == 0) {
        return {};
    }

    const Status validation = ValidateAdditionalRecords(source_count);
    if (!validation.has_value()) {
        return validation;
    }

    const ByteView source_records = source.encoded().subspan(WriteBatchHeaderSize);
    std::string stable_records;
    std::string_view records = AsStringView(source_records);
    if (this == &source) {
        stable_records.assign(records);
        records = stable_records;
    }
    const bool records_too_large = records.size() > encoded_.max_size() - encoded_.size();
    if (records_too_large) {     // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs more than string max_size
        return BatchTooLarge();  // GCOVR_EXCL_LINE: needs more than string max_size
    }
    AppendRecords(records, source_count);
    return {};
}

void EncodedWriteBatch::AppendTrusted(const EncodedWriteBatch& source) {
    assert(this != &source);
    const std::uint32_t source_count = source.count();
    if (source_count == 0) {
        return;
    }

    assert(source_count <= std::numeric_limits<std::uint32_t>::max() - count());
    assert(IsSequenceRangeValid(sequence(), count() + source_count));
    const std::string_view records = AsStringView(source.encoded().subspan(WriteBatchHeaderSize));
    assert(records.size() <= encoded_.max_size() - encoded_.size());
    AppendRecords(records, source_count);
}

Status EncodedWriteBatch::AppendRecord(ValueKind kind, ByteView key, ByteView value) {
    if (key.size() > MaximumKeySize) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs over 4 GiB
        return KeyTooLong();            // GCOVR_EXCL_LINE: needs a key over 4 GiB
    }
    const bool value_too_long = value.size() > std::numeric_limits<std::uint32_t>::max();
    if (value_too_long) {       // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs a value over 4 GiB
        return ValueTooLong();  // GCOVR_EXCL_LINE: needs a value over 4 GiB
    }

    const std::uint64_t record_size =
        std::uint64_t{1} + VarintLength(key.size()) + key.size() +
        (kind == ValueKind::Value ? std::uint64_t{VarintLength(value.size())} + value.size()
                                  : std::uint64_t{0});
    const bool record_too_large = record_size > encoded_.max_size() - encoded_.size();
    if (record_too_large) {      // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs more than string max_size
        return BatchTooLarge();  // GCOVR_EXCL_LINE: needs more than string max_size
    }

    const std::size_t old_size = encoded_.size();
    encoded_.resize(old_size + static_cast<std::size_t>(record_size));
    MutableByteView output =
        AsWritableBytes(std::span<char>(encoded_.data() + old_size, encoded_.size() - old_size));
    output.front() = static_cast<std::byte>(kind);
    output = output.subspan(1);
    EncodeBatchValue(output, key);
    if (kind == ValueKind::Value) {
        EncodeBatchValue(output, value);
    }
    assert(output.empty());
    return {};
}

Status EncodedWriteBatch::SetSequence(SequenceNumber sequence) {
    if (!IsSequenceRangeValid(sequence, count())) {
        return std::unexpected(
            Error::InvalidArgument("write batch sequence range exceeds 56 bits"));
    }
    MutableByteView bytes = AsWritableBytes(std::span<char>(encoded_.data(), encoded_.size()));
    EncodeFixed64(bytes.subspan<SequenceOffset, sizeof(SequenceNumber)>(), sequence);
    return {};
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

Status EncodedWriteBatch::ValidateAdditionalRecords(std::uint32_t additional) const {
    const std::uint32_t current_count = count();
    if (additional > std::numeric_limits<std::uint32_t>::max() - current_count) {
        return std::unexpected(Error::InvalidArgument("write batch record count exceeds uint32"));
    }

    const std::uint32_t new_count = current_count + additional;
    if (!IsSequenceRangeValid(sequence(), new_count)) {
        return std::unexpected(
            Error::InvalidArgument("write batch sequence range exceeds 56 bits"));
    }
    return {};
}

void EncodedWriteBatch::AppendRecords(std::string_view records, std::uint32_t count) {
    encoded_.append(records.data(), records.size());
    SetCount(this->count() + count);
}

void EncodedWriteBatch::SetCount(std::uint32_t count) noexcept {
    MutableByteView bytes = AsWritableBytes(std::span<char>(encoded_.data(), encoded_.size()));
    EncodeFixed32(bytes.subspan<CountOffset, sizeof(std::uint32_t)>(), count);
}

}  // namespace modern_leveldb
