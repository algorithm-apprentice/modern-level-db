#include "wal/wal_io.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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

constexpr std::uint64_t WalBlockSize64 = WalBlockSize;

std::uint64_t SaturatingSize(std::size_t value) noexcept {
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
    if (value > std::numeric_limits<std::uint64_t>::max()) {
      return std::numeric_limits<std::uint64_t>::max();
    }
  }
  return static_cast<std::uint64_t>(value);
}

std::uint64_t SaturatingAdd(std::uint64_t left,
                            std::uint64_t right) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

Error ClosedError(std::string_view operation) {
  return Error::InvalidArgument(std::string(operation) +
                                " called after WAL writer close");
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
  if (!status.has_value() && !first_error_.has_value()) {
    first_error_ = status.error();
  }
  if (first_error_.has_value()) {
    return std::unexpected(*first_error_);
  }
  return {};
}

Status WalWriter::Append(ByteView data) {
  return RememberError(file_->Append(data));
}

WalReader::WalReader(std::unique_ptr<SequentialFile> file,
                     WalReaderOptions options)
    : file_(std::move(file)),
      initial_offset_(options.initial_offset),
      resyncing_(options.initial_offset > 0) {
  if (file_ == nullptr) {
    terminal_error_ = Error::InvalidArgument("WAL reader requires a file");
  }
}

WalReadResult WalReader::ReadNext() {
  if (terminal_error_.has_value()) {
    return std::unexpected(*terminal_error_);
  }
  if (pending_record_.has_value()) {
    const WalLogicalRecord record = *pending_record_;
    pending_record_.reset();
    return ReturnRecord(record.data, record.offset);
  }
  if (exhausted_) {
    return std::optional<WalReadEvent>{};
  }
  if (!initialized_) {
    Status initialized = Initialize();
    if (!initialized.has_value()) {
      return ReturnTerminal(initialized.error());
    }
  }

  while (true) {
    if (block_position_ == block_size_) {
      Result<bool> filled = FillBlock();
      if (!filled.has_value()) {
        return ReturnTerminal(filled.error());
      }
      if (!*filled) {
        ClearPartial();
        exhausted_ = true;
        return std::optional<WalReadEvent>{};
      }
    }

    const std::size_t bytes_left = block_size_ - block_position_;
    if (bytes_left < WalHeaderSize) {
      block_position_ = block_size_;
      if (eof_seen_) {
        ClearPartial();
        exhausted_ = true;
        return std::optional<WalReadEvent>{};
      }
      continue;
    }

    const std::uint64_t physical_offset =
        block_start_offset_ + static_cast<std::uint64_t>(block_position_);
    const ByteView unread =
        ByteView(block_).subspan(block_position_, bytes_left);
    WalDecodeResult decoded = DecodeWalFragment(unread);
    if (!decoded.has_value()) {
      const WalDecodeError& decode_error = decoded.error();
      if (eof_seen_ &&
          (decode_error.failure == WalDecodeFailure::TruncatedPayload ||
           decode_error.failure == WalDecodeFailure::PayloadTooLarge)) {
        block_position_ = block_size_;
        ClearPartial();
        exhausted_ = true;
        return std::optional<WalReadEvent>{};
      }

      std::size_t discarded = 0;
      if (decode_error.recovery == WalRecoveryAction::SkipPhysicalRecord) {
        discarded = decode_error.encoded_size;
        block_position_ += discarded;
      } else {
        discarded = bytes_left;
        block_position_ = block_size_;
      }

      if (physical_offset < initial_offset_) {
        ClearPartial();
        continue;
      }

      std::uint64_t dropped = SaturatingSize(discarded);
      const std::uint64_t error_offset =
          in_fragmented_record_ ? partial_record_offset_ : physical_offset;
      if (in_fragmented_record_) {
        dropped =
            SaturatingAdd(dropped, SaturatingSize(scratch_.size()));
      }
      Error error = decode_error.error;
      ClearPartial();
      return ReturnCorruption(std::move(error), dropped, error_offset);
    }

    const WalDecodeOutcome& outcome = *decoded;
    if (outcome.kind == WalDecodeKind::EndOfBlock) {
      block_position_ = block_size_;
      if (physical_offset < initial_offset_) {
        ClearPartial();
        continue;
      }
      if (in_fragmented_record_ && !scratch_.empty()) {
        const std::uint64_t dropped = SaturatingSize(scratch_.size());
        const std::uint64_t error_offset = partial_record_offset_;
        ClearPartial();
        return ReturnCorruption(
            Error::Corruption(
                "WAL fragmented record ended at a zero marker"),
            dropped, error_offset);
      }
      ClearPartial();
      continue;
    }

    block_position_ += outcome.encoded_size;
    if (physical_offset < initial_offset_) {
      ClearPartial();
      continue;
    }

    if (resyncing_) {
      if (outcome.type == WalRecordType::Middle) {
        continue;
      }
      if (outcome.type == WalRecordType::Last) {
        resyncing_ = false;
        continue;
      }
      resyncing_ = false;
    }

    switch (outcome.type) {
      case WalRecordType::Full:
        if (in_fragmented_record_ && !scratch_.empty()) {
          const std::uint64_t dropped = SaturatingSize(scratch_.size());
          const std::uint64_t error_offset = partial_record_offset_;
          pending_record_ =
              WalLogicalRecord{.data = outcome.payload,
                               .offset = physical_offset};
          ClearPartial();
          return ReturnCorruption(
              Error::Corruption(
                  "WAL fragmented record was interrupted by a full record"),
              dropped, error_offset);
        }
        ClearPartial();
        return ReturnRecord(outcome.payload, physical_offset);

      case WalRecordType::First:
        if (in_fragmented_record_ && !scratch_.empty()) {
          const std::uint64_t dropped = SaturatingSize(scratch_.size());
          const std::uint64_t error_offset = partial_record_offset_;
          scratch_.assign(outcome.payload.begin(), outcome.payload.end());
          partial_record_offset_ = physical_offset;
          in_fragmented_record_ = true;
          return ReturnCorruption(
              Error::Corruption(
                  "WAL fragmented record was interrupted by a first fragment"),
              dropped, error_offset);
        }
        scratch_.assign(outcome.payload.begin(), outcome.payload.end());
        partial_record_offset_ = physical_offset;
        in_fragmented_record_ = true;
        break;

      case WalRecordType::Middle:
        if (!in_fragmented_record_) {
          return ReturnCorruption(
              Error::Corruption(
                  "WAL middle fragment has no starting fragment"),
              SaturatingSize(outcome.payload.size()), physical_offset);
        }
        scratch_.insert(scratch_.end(), outcome.payload.begin(),
                        outcome.payload.end());
        break;

      case WalRecordType::Last:
        if (!in_fragmented_record_) {
          return ReturnCorruption(
              Error::Corruption(
                  "WAL last fragment has no starting fragment"),
              SaturatingSize(outcome.payload.size()), physical_offset);
        }
        scratch_.insert(scratch_.end(), outcome.payload.begin(),
                        outcome.payload.end());
        in_fragmented_record_ = false;
        return ReturnRecord(scratch_, partial_record_offset_);

      case WalRecordType::Zero:
        break;
    }
  }
}

Status WalReader::Initialize() {
  const std::uint64_t offset_in_block = initial_offset_ % WalBlockSize64;
  std::uint64_t block_start = initial_offset_ - offset_in_block;
  if (offset_in_block > WalBlockSize64 - 6U) {
    if (block_start >
        std::numeric_limits<std::uint64_t>::max() - WalBlockSize64) {
      return std::unexpected(
          Error::InvalidArgument("WAL initial offset overflows block start"));
    }
    block_start += WalBlockSize64;
  }

  if (block_start != 0) {
    Status skipped = file_->Skip(block_start);
    if (!skipped.has_value()) {
      return skipped;
    }
  }
  next_block_offset_ = block_start;
  initialized_ = true;
  return {};
}

Result<bool> WalReader::FillBlock() {
  if (eof_seen_) {
    return false;
  }

  block_start_offset_ = next_block_offset_;
  block_size_ = 0;
  block_position_ = 0;

  while (block_size_ < WalBlockSize) {
    MutableByteView output =
        MutableByteView(block_).subspan(block_size_);
    Result<std::size_t> read = file_->Read(output);
    if (!read.has_value()) {
      return std::unexpected(read.error());
    }
    if (*read > output.size()) {
      return std::unexpected(
          Error::Io("sequential file returned an oversized WAL read"));
    }
    if (*read == 0) {
      eof_seen_ = true;
      break;
    }
    if (*read >
        std::numeric_limits<std::uint64_t>::max() - next_block_offset_) {
      return std::unexpected(
          Error::InvalidArgument("WAL read offset overflow"));
    }
    block_size_ += *read;
    next_block_offset_ += *read;
  }

  return block_size_ != 0;
}

WalReadResult WalReader::ReturnTerminal(Error error) {
  if (!terminal_error_.has_value()) {
    terminal_error_ = std::move(error);
  }
  return std::unexpected(*terminal_error_);
}

WalReadResult WalReader::ReturnRecord(ByteView data, std::uint64_t offset) {
  return std::optional<WalReadEvent>(
      WalReadEvent(WalLogicalRecord{.data = data, .offset = offset}));
}

WalReadResult WalReader::ReturnCorruption(Error error,
                                          std::uint64_t dropped_bytes,
                                          std::uint64_t offset) {
  return std::optional<WalReadEvent>(WalReadEvent(WalCorruption{
      .error = std::move(error),
      .dropped_bytes = dropped_bytes,
      .offset = offset,
  }));
}

void WalReader::ClearPartial() noexcept {
  scratch_.clear();
  in_fragmented_record_ = false;
  partial_record_offset_ = 0;
}

}  // namespace modern_leveldb
