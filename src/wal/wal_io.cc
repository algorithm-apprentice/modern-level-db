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

Error ClosedError(std::string_view operation) {
  return Error::InvalidArgument(std::string(operation) +
                                " called after WAL writer close");
}

WalReadResult Event(WalReadEvent event) {
  return std::optional<WalReadEvent>(std::move(event));
}

}  // namespace

WalWriter::WalWriter(std::unique_ptr<WritableFile> file,
                     std::uint64_t initial_file_size)
    : file_(std::move(file)), fragmenter_(0) {
  if (file_ == nullptr) {
    first_error_ = Error::InvalidArgument("WAL writer requires a file");
  }

  const std::size_t block_offset =
      static_cast<std::size_t>(initial_file_size % WalBlockSize);
  if (initial_file_size != 0 && block_offset != 0) {
    reopen_padding_ = WalBlockSize - block_offset;
  }
}

Status WalWriter::AddRecord(ByteView logical_record) {
  Status usable = CheckUsable("add record");
  if (!usable.has_value()) {
    return usable;
  }

  const std::vector<WalFragment> fragments =
      fragmenter_.Fragment(logical_record);
  static constexpr std::array<std::byte, WalBlockSize> Zeroes{};

  if (reopen_padding_ != 0) {
    Status padding = Append(ByteView(Zeroes).first(reopen_padding_));
    if (!padding.has_value()) {
      return padding;
    }
    reopen_padding_ = 0;
  }

  for (const WalFragment& fragment : fragments) {
    if (fragment.padding_before != 0) {
      Status padding =
          Append(ByteView(Zeroes).first(fragment.padding_before));
      if (!padding.has_value()) {
        return padding;
      }
    }

    Status header = Append(fragment.header);
    if (!header.has_value()) {
      return header;
    }
    if (!fragment.payload.empty()) {
      Status payload = Append(fragment.payload);
      if (!payload.has_value()) {
        return payload;
      }
    }
  }

  return RememberError(file_->Flush());
}

Status WalWriter::Sync() {
  Status usable = CheckUsable("sync");
  if (!usable.has_value()) {
    return usable;
  }
  return RememberError(file_->Sync());
}

Status WalWriter::Close() {
  if (closed_) {
    return std::unexpected(ClosedError("close"));
  }
  closed_ = true;

  Status close_status;
  if (file_ != nullptr) {
    close_status = file_->Close();
  }
  if (!close_status.has_value() && !first_error_.has_value()) {
    first_error_ = close_status.error();
  }
  if (first_error_.has_value()) {
    return std::unexpected(*first_error_);
  }
  return {};
}

Status WalWriter::CheckUsable(std::string_view operation) const {
  if (closed_) {
    return std::unexpected(ClosedError(operation));
  }
  if (first_error_.has_value()) {
    return std::unexpected(*first_error_);
  }
  return {};
}

Status WalWriter::RememberError(Status status) {
  if (!status.has_value()) {
    first_error_ = status.error();
  }
  return status;
}

Status WalWriter::Append(ByteView data) {
  return RememberError(file_->Append(data));
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
      if (in_fragmented_record_) {
        corruption->dropped_bytes += scratch_.size();
        corruption->offset = partial_record_offset_;
      }
      ClearPartial();
      return Event(std::move(*corruption));
    }

    const PhysicalFragment& fragment = std::get<PhysicalFragment>(**physical);
    switch (fragment.type) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/5 implicit default of an exhaustive switch
      case WalRecordType::Zero:
        if (HasPartialPayload()) {
          return AbandonPartial("WAL fragmented record ended at a zero marker");
        }
        ClearPartial();
        continue;

      case WalRecordType::Full:
        if (HasPartialPayload()) {
          // Leave this fragment unconsumed so the next call returns it.
          block_position_ = fragment.block_position;
          return AbandonPartial("WAL fragmented record was interrupted by a full record");
        }
        ClearPartial();
        return Event(WalLogicalRecord{.data = fragment.payload, .offset = fragment.offset});

      case WalRecordType::First:
        if (HasPartialPayload()) {
          block_position_ = fragment.block_position;
          return AbandonPartial("WAL fragmented record was interrupted by a first fragment");
        }
        scratch_.assign(fragment.payload.begin(), fragment.payload.end());
        partial_record_offset_ = fragment.offset;
        in_fragmented_record_ = true;
        continue;

      case WalRecordType::Middle:
      case WalRecordType::Last:
        if (!in_fragmented_record_) {
          return Event(WalCorruption{
              .error = Error::Corruption("WAL fragment has no starting fragment"),
              .dropped_bytes = fragment.payload.size(),
              .offset = fragment.offset,
          });
        }
        scratch_.insert(scratch_.end(), fragment.payload.begin(), fragment.payload.end());
        if (fragment.type == WalRecordType::Middle) {
          continue;
        }
        in_fragmented_record_ = false;
        return Event(WalLogicalRecord{.data = scratch_, .offset = partial_record_offset_});
    }
  }
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

    const std::uint64_t offset = block_start_offset_ + position;
    WalDecodeResult decoded = DecodeWalFragment(ByteView(block_).subspan(position, remaining));
    if (!decoded.has_value()) {
      WalDecodeError& failure = decoded.error();
      const std::size_t discarded = failure.recovery == WalRecoveryAction::SkipPhysicalRecord
                                        ? failure.encoded_size
                                        : remaining;
      block_position_ += discarded;
      // A payload extending past the final partial block is a crash-truncated tail.
      const bool truncated_tail =
          eof_seen_ && (failure.failure == WalDecodeFailure::TruncatedPayload ||
                        failure.failure == WalDecodeFailure::PayloadTooLarge);
      if (truncated_tail) {
        continue;
      }
      return PhysicalRead(WalCorruption{
          .error = std::move(failure.error),
          .dropped_bytes = discarded,
          .offset = offset,
      });
    }

    // A zero marker discards the rest of its block.
    block_position_ = decoded->kind == WalDecodeKind::EndOfBlock
                          ? block_size_
                          : position + decoded->encoded_size;
    return PhysicalRead(PhysicalFragment{
        .type = decoded->type,
        .payload = decoded->payload,
        .offset = offset,
        .block_position = position,
    });
  }
}

Result<bool> WalReader::FillBlock() {
  if (eof_seen_) {
    return false;
  }

  block_start_offset_ += block_size_;
  block_size_ = 0;
  block_position_ = 0;

  while (block_size_ < WalBlockSize) {
    MutableByteView output = MutableByteView(block_).subspan(block_size_);
    Result<std::size_t> read = file_->Read(output);
    if (!read.has_value()) {
      return std::unexpected(read.error());
    }
    if (*read > output.size()) {
      return std::unexpected(Error::Io("sequential file returned an oversized WAL read"));
    }
    if (*read == 0) {
      eof_seen_ = true;
      break;
    }
    block_size_ += *read;
  }

  return block_size_ != 0;
}

WalReadResult WalReader::AbandonPartial(std::string_view reason) {
  WalCorruption corruption{
      .error = Error::Corruption(std::string(reason)),
      .dropped_bytes = scratch_.size(),
      .offset = partial_record_offset_,
  };
  ClearPartial();
  return Event(std::move(corruption));
}

void WalReader::ClearPartial() noexcept {
  scratch_.clear();
  in_fragmented_record_ = false;
  partial_record_offset_ = 0;
}

}  // namespace modern_leveldb
