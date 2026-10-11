#include "wal/wal_io.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "format/wal_format.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"

namespace modern_leveldb {
namespace {

WalReadResult Event(WalReadEvent event) { return std::optional<WalReadEvent>(std::move(event)); }

}  // namespace

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
WalWriter::WalWriter(std::unique_ptr<WritableFile> file, std::uint64_t initial_file_size)
    : file_(std::move(file)), fragmenter_(0) {
    const std::size_t block_offset = static_cast<std::size_t>(initial_file_size % WalBlockSize);
    if (initial_file_size != 0 && block_offset != 0) {
        reopen_padding_ = WalBlockSize - block_offset;
    }
}
// GCOVR_EXCL_STOP

Status WalWriter::AddRecord(ByteView logical_record) {
    auto fragments = fragmenter_.Fragment(logical_record);
    Status reopened = PadReopenedBlock();
    if (!reopened.has_value()) {
        return reopened;
    }

    while (const std::optional<WalFragment> fragment = fragments.Next()) {
        Status written = WriteFragment(*fragment);
        if (!written.has_value()) {
            return written;
        }
    }

    return file_->Flush();
}

Status WalWriter::Sync() { return file_->Sync(); }

Status WalWriter::Close() { return file_->Close(); }

Status WalWriter::AppendPadding(std::size_t size) {
    static constexpr std::array<std::byte, WalBlockSize> Zeroes{};
    return file_->Append(ByteView(Zeroes).first(size));
}

Status WalWriter::PadReopenedBlock() {
    if (reopen_padding_ != 0) {
        Status padding = AppendPadding(reopen_padding_);
        if (!padding.has_value()) {
            return padding;
        }
        reopen_padding_ = 0;
    }
    return {};
}

Status WalWriter::WriteFragment(const WalFragment& fragment) {
    if (fragment.padding_before != 0) {
        Status padding = AppendPadding(fragment.padding_before);
        if (!padding.has_value()) {
            return padding;
        }
    }
    Status header = file_->Append(fragment.header);
    if (!header.has_value()) {
        return header;
    }
    if (!fragment.payload.empty()) {
        Status payload = file_->Append(fragment.payload);
        if (!payload.has_value()) {
            return payload;
        }
    }
    return {};
}

WalReader::WalReader(std::unique_ptr<SequentialFile> file) : file_(std::move(file)) {
    if (file_ == nullptr) {
        terminal_error_ = Error::InvalidArgument("WAL reader requires a file");
    }
}

WalReadResult WalReader::ReadNext() {
    if (terminal_error_.has_value()) {
        return std::unexpected(*terminal_error_);
    }

    while (true) {
        Result<std::optional<PhysicalRead>> physical = ReadPhysical();
        if (!physical.has_value()) {
            terminal_error_ = std::move(physical.error());
            return std::unexpected(*terminal_error_);
        }
        if (!physical->has_value()) {
            // An incomplete final record is a crash-truncated tail, not corruption.
            ClearPartial();
            return std::optional<WalReadEvent>{};
        }

        if (auto* corruption = std::get_if<WalCorruption>(&**physical)) {
            return Event(ConsumeCorruption(std::move(*corruption)));
        }

        std::optional<WalReadEvent> event = ConsumeFragment(std::get<PhysicalFragment>(**physical));
        if (event.has_value()) {
            return Event(std::move(*event));
        }
    }
}

WalReadEvent WalReader::ConsumeCorruption(WalCorruption corruption) {
    if (in_fragmented_record_) {
        corruption.dropped_bytes += scratch_.size();
        corruption.offset = partial_record_offset_;
    }
    ClearPartial();
    return corruption;
}

std::optional<WalReadEvent> WalReader::ConsumeFragment(const PhysicalFragment& fragment) {
    switch (fragment.type) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/5 exhaustive switch default
        case WalRecordType::Zero:
            return ConsumeZero();
        case WalRecordType::Full:
            return ConsumeFull(fragment);
        case WalRecordType::First:
            return ConsumeFirst(fragment);
        case WalRecordType::Middle:
        case WalRecordType::Last:
            return ConsumeContinuation(fragment);
    }
    std::unreachable();
}

std::optional<WalReadEvent> WalReader::ConsumeZero() {
    if (HasPartialPayload()) {
        return AbandonPartial("WAL fragmented record ended at a zero marker");
    }
    ClearPartial();
    return std::nullopt;
}

std::optional<WalReadEvent> WalReader::ConsumeFull(const PhysicalFragment& fragment) {
    if (HasPartialPayload()) {
        // Leave this fragment unconsumed so the next call returns it.
        block_position_ = fragment.block_position;
        return AbandonPartial("WAL fragmented record was interrupted by a full record");
    }
    ClearPartial();
    return WalLogicalRecord{.data = fragment.payload, .offset = fragment.offset};
}

std::optional<WalReadEvent> WalReader::ConsumeFirst(const PhysicalFragment& fragment) {
    if (HasPartialPayload()) {
        block_position_ = fragment.block_position;
        return AbandonPartial("WAL fragmented record was interrupted by a first fragment");
    }
    scratch_.assign(fragment.payload.begin(), fragment.payload.end());
    partial_record_offset_ = fragment.offset;
    in_fragmented_record_ = true;
    return std::nullopt;
}

std::optional<WalReadEvent> WalReader::ConsumeContinuation(const PhysicalFragment& fragment) {
    if (!in_fragmented_record_) {
        WalCorruption corruption{
            .error = Error::Corruption("WAL fragment has no starting fragment"),
            .dropped_bytes = fragment.payload.size(),
            .offset = fragment.offset,
        };
        return corruption;
    }
    scratch_.insert(scratch_.end(), fragment.payload.begin(), fragment.payload.end());
    if (fragment.type == WalRecordType::Middle) {
        return std::nullopt;
    }
    in_fragmented_record_ = false;
    return WalLogicalRecord{.data = scratch_, .offset = partial_record_offset_};
}

Result<std::optional<WalReader::PhysicalRead>> WalReader::ReadPhysical() {
    while (true) {
        if (block_position_ == block_size_) {
            Result<bool> filled = FillBlock();
            if (!filled.has_value()) {
                return std::unexpected(filled.error());
            }
            if (!*filled) {
                return std::nullopt;
            }
        }

        const std::size_t position = block_position_;
        const std::size_t remaining = block_size_ - position;
        if (remaining < WalHeaderSize) {
            // Zero trailer of a full block, or a crash-truncated header at EOF.
            block_position_ = block_size_;
            continue;
        }

        std::optional<PhysicalRead> decoded = DecodePhysical(position, remaining);
        if (decoded.has_value()) {
            return decoded;
        }
    }
}

std::optional<WalReader::PhysicalRead> WalReader::DecodePhysical(std::size_t position,
                                                                 std::size_t remaining) {
    const std::uint64_t offset = block_start_offset_ + position;
    WalDecodeResult decoded = DecodeWalFragment(ByteView(block_).subspan(position, remaining));
    if (!decoded.has_value()) {
        return RecoverPhysicalFailure(decoded.error(), offset, remaining);
    }

    // A zero marker discards the rest of its block.
    block_position_ =
        decoded->kind == WalDecodeKind::EndOfBlock ? block_size_ : position + decoded->encoded_size;
    return PhysicalRead(PhysicalFragment{
        .type = decoded->type,
        .payload = decoded->payload,
        .offset = offset,
        .block_position = position,
    });
}

std::optional<WalReader::PhysicalRead> WalReader::RecoverPhysicalFailure(WalDecodeError& failure,
                                                                         std::uint64_t offset,
                                                                         std::size_t remaining) {
    const std::size_t discarded = failure.recovery == WalRecoveryAction::SkipPhysicalRecord
                                      ? failure.encoded_size
                                      : remaining;
    block_position_ += discarded;
    // A payload extending past the final partial block is a crash-truncated tail.
    const bool truncated_tail =
        eof_seen_ && (failure.failure == WalDecodeFailure::TruncatedPayload ||
                      failure.failure == WalDecodeFailure::PayloadTooLarge);
    if (truncated_tail) {
        return std::nullopt;
    }
    return PhysicalRead(WalCorruption{
        .error = std::move(failure.error),
        .dropped_bytes = discarded,
        .offset = offset,
    });
}

Result<bool> WalReader::FillBlock() {
    if (eof_seen_) {
        return false;
    }

    BeginBlock();
    while (block_size_ < WalBlockSize) {
        Result<std::size_t> read = ReadBlockChunk();
        if (!read.has_value()) {
            return std::unexpected(std::move(read).error());
        }
        AcceptBlockChunk(*read);
        if (*read == 0) {
            break;
        }
    }

    return block_size_ != 0;
}

void WalReader::BeginBlock() noexcept {
    block_start_offset_ += block_size_;
    block_size_ = 0;
    block_position_ = 0;
}

Result<std::size_t> WalReader::ReadBlockChunk() {
    MutableByteView output = MutableByteView(block_).subspan(block_size_);
    Result<std::size_t> read = file_->Read(output);
    if (!read.has_value()) {
        return std::unexpected(read.error());
    }
    if (*read > output.size()) {
        return std::unexpected(Error::Io("sequential file returned an oversized WAL read"));
    }
    return read;
}

void WalReader::AcceptBlockChunk(std::size_t count) noexcept {
    if (count == 0) {
        eof_seen_ = true;
    } else {
        block_size_ += count;
    }
}

WalReadEvent WalReader::AbandonPartial(std::string_view reason) {
    WalCorruption corruption{
        .error = Error::Corruption(std::string(reason)),
        .dropped_bytes = scratch_.size(),
        .offset = partial_record_offset_,
    };
    ClearPartial();
    return corruption;
}

void WalReader::ClearPartial() noexcept {
    scratch_.clear();
    in_fragmented_record_ = false;
    partial_record_offset_ = 0;
}

}  // namespace modern_leveldb
