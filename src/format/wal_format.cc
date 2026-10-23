#include "format/wal_format.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>

#include "base/coding_internal.h"
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

constexpr std::size_t TypeIndex(WalRecordType type) noexcept {
    return static_cast<std::size_t>(type) - static_cast<std::size_t>(WalRecordType::Full);
}

constexpr WalRecordType SelectFragmentType(bool begin, bool end) noexcept {
    if (begin && end) {
        return WalRecordType::Full;
    }
    if (begin) {
        return WalRecordType::First;
    }
    if (end) {
        return WalRecordType::Last;
    }
    return WalRecordType::Middle;
}

std::array<std::byte, WalHeaderSize> EncodeHeader(WalRecordType type, ByteView payload,
                                                  std::uint32_t type_crc) noexcept {
    const std::byte type_byte = static_cast<std::byte>(type);
    std::uint32_t crc = ExtendCrc32c(type_crc, payload);
    crc = MaskCrc32c(crc);

    std::array<std::byte, WalHeaderSize> header{};
    EncodeFixedTrusted(std::span(header).first<sizeof(crc)>(), crc);
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

std::unexpected<WalDecodeError> DecodeFailure(WalDecodeFailure failure, WalRecoveryAction recovery,
                                              std::string message, std::size_t encoded_size = 0) {
    return std::unexpected(WalDecodeError{
        .error = Error::Corruption(std::move(message)),
        .failure = failure,
        .recovery = recovery,
        .encoded_size = encoded_size,
    });
}

std::expected<std::size_t, WalDecodeError> ReadWalPayloadSize(ByteView encoded) {
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
    if (encoded.size() < WalHeaderSize + payload_size) {
        return DecodeFailure(WalDecodeFailure::TruncatedPayload, WalRecoveryAction::DropBlock,
                             "WAL physical record has a truncated payload");
    }
    return payload_size;
}

bool WalChecksumMatches(ByteView encoded, ByteView payload) noexcept {
    const std::uint32_t expected = UnmaskCrc32c(DecodeStoredCrc(encoded));
    const std::byte type_byte = encoded[6];
    const std::uint32_t actual = ExtendCrc32c(Crc32c(ByteView(&type_byte, 1)), payload);
    return actual == expected;
}

WalFragment BuildWalFragment(std::size_t padding_before, WalRecordType type, ByteView payload,
                             std::uint32_t type_crc) noexcept {
    return {
        .padding_before = padding_before,
        .header = EncodeHeader(type, payload, type_crc),
        .payload = payload,
    };
}

}  // namespace

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
WalFragmenter::WalFragmenter(std::uint64_t initial_file_size) noexcept
    : block_offset_(static_cast<std::size_t>(initial_file_size % WalBlockSize)) {
    for (std::size_t index = 0; index < type_crc_.size(); ++index) {
        const auto type =
            static_cast<WalRecordType>(index + static_cast<std::size_t>(WalRecordType::Full));
        const std::byte type_byte = static_cast<std::byte>(type);
        type_crc_[index] = Crc32c(ByteView(&type_byte, 1));
    }
}
// GCOVR_EXCL_STOP

std::size_t WalFragmenter::Cursor::TakePadding() noexcept {
    const std::size_t bytes_left_in_block = WalBlockSize - *block_offset_;
    if (bytes_left_in_block >= WalHeaderSize) {
        return 0;
    }
    *block_offset_ = 0;
    return bytes_left_in_block;
}

std::size_t WalFragmenter::Cursor::FragmentPayloadSize() const noexcept {
    const std::size_t available = WalBlockSize - *block_offset_ - WalHeaderSize;
    return std::min(remaining_.size(), available);
}

void WalFragmenter::Cursor::Advance(std::size_t fragment_size, bool end) noexcept {
    *block_offset_ = (*block_offset_ + WalHeaderSize + fragment_size) % WalBlockSize;
    remaining_ = remaining_.subspan(fragment_size);
    begin_ = false;
    done_ = end;
}

std::optional<WalFragment> WalFragmenter::Cursor::Next() noexcept {
    WalFragment fragment;
    if (!Next(fragment)) {
        return std::nullopt;
    }
    return fragment;
}

bool WalFragmenter::Cursor::Next(WalFragment& fragment) noexcept {
    if (done_) {
        return false;
    }

    const std::size_t padding_before = TakePadding();
    // Exactly one header of space can emit an empty First fragment for a nonempty
    // record; the following block carries its payload. A shorter tail is padding.
    const std::size_t fragment_size = FragmentPayloadSize();
    const bool end = fragment_size == remaining_.size();
    const WalRecordType type = SelectFragmentType(begin_, end);
    const ByteView payload = remaining_.first(fragment_size);
    Advance(fragment_size, end);
    fragment = BuildWalFragment(padding_before, type, payload, (*type_crc_)[TypeIndex(type)]);
    return true;
}

WalDecodeResult DecodeWalFragment(ByteView encoded, bool verify_checksum) {
    std::expected<std::size_t, WalDecodeError> payload_size = ReadWalPayloadSize(encoded);
    if (!payload_size.has_value()) {
        return std::unexpected(std::move(payload_size).error());
    }

    const std::size_t encoded_size = WalHeaderSize + *payload_size;
    const auto type = static_cast<WalRecordType>(std::to_integer<std::uint8_t>(encoded[6]));
    if (type == WalRecordType::Zero && *payload_size == 0) {
        return WalDecodeOutcome{
            .kind = WalDecodeKind::EndOfBlock,
            .type = WalRecordType::Zero,
            .payload = {},
            .encoded_size = WalHeaderSize,
        };
    }

    const ByteView payload = encoded.subspan(WalHeaderSize, *payload_size);
    if (verify_checksum && !WalChecksumMatches(encoded, payload)) {
        return DecodeFailure(WalDecodeFailure::ChecksumMismatch, WalRecoveryAction::DropBlock,
                             "WAL physical record checksum mismatch");
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
