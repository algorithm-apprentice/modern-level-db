#include "metadata/version_edit.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

enum class Tag : std::uint32_t {
  Comparator = 1,
  LogNumber = 2,
  NextFileNumber = 3,
  LastSequence = 4,
  CompactPointer = 5,
  DeletedFile = 6,
  NewFile = 7,
  PrevLogNumber = 9,
};

constexpr std::size_t MaximumFieldSize =
    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

// GCOVR_EXCL_START: only fields larger than 4 GiB reach this function
std::unexpected<Error> FieldTooLarge(std::string_view field) {
  return std::unexpected(Error::InvalidArgument(std::string(field) + " exceeds uint32 length"));
}
// GCOVR_EXCL_STOP

std::unexpected<Error> Malformed(std::string_view field) {
  return std::unexpected(Error::Corruption("malformed version edit " + std::string(field)));
}

void AppendTag(std::vector<std::byte>& output, Tag tag) {
  AppendVarint32(output, static_cast<std::uint32_t>(tag));
}

void AppendNumberField(std::vector<std::byte>& output, Tag tag,
                       std::optional<std::uint64_t> number) {
  if (number.has_value()) {
    AppendTag(output, tag);
    AppendVarint64(output, *number);
  }
}

void AppendField(std::vector<std::byte>& output, ByteView value) {
  const Status appended = AppendLengthPrefixed(output, value);
  assert(appended.has_value());
  (void)appended;
}

// Rejects moved-from keys, whose encoding is empty.
[[nodiscard]] bool IsValidInternalKey(const InternalKey& key) {
  return ParseInternalKey(key.encoded()).has_value();
}

[[nodiscard]] bool ReadNumber(ByteView& input, std::uint64_t& number) {
  const Result<std::uint64_t> value = ConsumeVarint64(input);
  if (!value.has_value()) {
    return false;
  }
  number = *value;
  return true;
}

[[nodiscard]] bool ReadFileNumber(ByteView& input, std::uint64_t& number) {
  return ReadNumber(input, number) && number != 0;
}

[[nodiscard]] bool ReadLevel(ByteView& input, std::uint32_t& level) {
  const Result<std::uint32_t> value = ConsumeVarint32(input);
  if (!value.has_value() || *value >= NumLevels) {
    return false;
  }
  level = *value;
  return true;
}

[[nodiscard]] bool ReadInternalKey(ByteView& input, std::optional<InternalKey>& key) {
  const Result<ByteView> encoded = ConsumeLengthPrefixed(input);
  if (!encoded.has_value()) {
    return false;
  }
  Result<InternalKey> decoded = InternalKey::Decode(*encoded);
  if (!decoded.has_value()) {
    return false;
  }
  key.emplace(std::move(*decoded));
  return true;
}

}  // namespace

Status VersionEdit::SetComparatorName(std::string_view name) {
  if (name.size() > MaximumFieldSize) {       // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs over 4 GiB
    return FieldTooLarge("comparator name");  // GCOVR_EXCL_LINE: needs over 4 GiB
  }
  comparator_name_ = std::string(name);
  return {};
}

Status VersionEdit::SetNextFileNumber(std::uint64_t number) {
  if (number == 0) {
    return std::unexpected(Error::InvalidArgument("next file number must be nonzero"));
  }
  next_file_number_ = number;
  return {};
}

Status VersionEdit::SetLastSequence(SequenceNumber sequence) {
  if (sequence > MaxSequenceNumber) {
    return std::unexpected(Error::InvalidArgument("last sequence exceeds 56 bits"));
  }
  last_sequence_ = sequence;
  return {};
}

Status VersionEdit::AddCompactPointer(std::uint32_t level, InternalKey key) {
  if (level >= NumLevels) {
    return std::unexpected(Error::InvalidArgument("compact pointer level is out of range"));
  }
  if (!IsValidInternalKey(key)) {
    return std::unexpected(Error::InvalidArgument("compact pointer key is malformed"));
  }
  if (key.encoded().size() > MaximumFieldSize) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs over 4 GiB
    return FieldTooLarge("compact pointer key");  // GCOVR_EXCL_LINE: needs over 4 GiB
  }
  compact_pointers_.push_back(CompactPointer{.level = level, .key = std::move(key)});
  return {};
}

Status VersionEdit::AddFile(std::uint32_t level, FileMetadata file) {
  if (level >= NumLevels) {
    return std::unexpected(Error::InvalidArgument("new file level is out of range"));
  }
  if (file.number == 0) {
    return std::unexpected(Error::InvalidArgument("new file number must be nonzero"));
  }
  if (!IsValidInternalKey(file.smallest) || !IsValidInternalKey(file.largest)) {
    return std::unexpected(Error::InvalidArgument("new file key is malformed"));
  }
  const std::size_t longest_key =
      std::max(file.smallest.encoded().size(), file.largest.encoded().size());
  if (longest_key > MaximumFieldSize) {    // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs over 4 GiB
    return FieldTooLarge("new file key");  // GCOVR_EXCL_LINE: needs over 4 GiB
  }
  new_files_.push_back(NewFile{.level = level, .file = std::move(file)});
  return {};
}

Status VersionEdit::RemoveFile(std::uint32_t level, std::uint64_t number) {
  if (level >= NumLevels) {
    return std::unexpected(Error::InvalidArgument("deleted file level is out of range"));
  }
  if (number == 0) {
    return std::unexpected(Error::InvalidArgument("deleted file number must be nonzero"));
  }
  deleted_files_.insert(DeletedFile{.level = level, .number = number});
  return {};
}

std::vector<std::byte> VersionEdit::Encode() const {
  std::vector<std::byte> output;
  if (comparator_name_.has_value()) {
    AppendTag(output, Tag::Comparator);
    AppendField(output, AsBytes(*comparator_name_));
  }
  AppendNumberField(output, Tag::LogNumber, log_number_);
  AppendNumberField(output, Tag::PrevLogNumber, prev_log_number_);
  AppendNumberField(output, Tag::NextFileNumber, next_file_number_);
  AppendNumberField(output, Tag::LastSequence, last_sequence_);
  for (const CompactPointer& pointer : compact_pointers_) {
    AppendTag(output, Tag::CompactPointer);
    AppendVarint32(output, pointer.level);
    AppendField(output, pointer.key.encoded());
  }
  for (const DeletedFile& file : deleted_files_) {
    AppendTag(output, Tag::DeletedFile);
    AppendVarint32(output, file.level);
    AppendVarint64(output, file.number);
  }
  for (const NewFile& entry : new_files_) {
    AppendTag(output, Tag::NewFile);
    AppendVarint32(output, entry.level);
    AppendVarint64(output, entry.file.number);
    AppendVarint64(output, entry.file.file_size);
    AppendField(output, entry.file.smallest.encoded());
    AppendField(output, entry.file.largest.encoded());
  }
  return output;
}

Result<VersionEdit> VersionEdit::Decode(ByteView encoded) {
  VersionEdit edit;
  while (!encoded.empty()) {
    const Result<std::uint32_t> tag = ConsumeVarint32(encoded);
    if (!tag.has_value()) {
      return Malformed("tag");
    }
    Status field = edit.DecodeField(*tag, encoded);
    if (!field.has_value()) {
      return std::unexpected(std::move(field).error());
    }
  }
  return edit;
}

Status VersionEdit::DecodeField(std::uint32_t tag, ByteView& input) {
  std::uint32_t level = 0;
  std::uint64_t number = 0;
  switch (static_cast<Tag>(tag)) {
    case Tag::Comparator: {
      const Result<ByteView> name = ConsumeLengthPrefixed(input);
      if (!name.has_value()) {
        return Malformed("comparator name");
      }
      comparator_name_.emplace(AsStringView(*name));
      return {};
    }
    case Tag::LogNumber:
      if (!ReadNumber(input, number)) {
        return Malformed("log number");
      }
      log_number_ = number;
      return {};
    case Tag::PrevLogNumber:
      if (!ReadNumber(input, number)) {
        return Malformed("previous log number");
      }
      prev_log_number_ = number;
      return {};
    case Tag::NextFileNumber:
      if (!ReadFileNumber(input, number)) {
        return Malformed("next file number");
      }
      next_file_number_ = number;
      return {};
    case Tag::LastSequence:
      if (!ReadNumber(input, number) || number > MaxSequenceNumber) {
        return Malformed("last sequence");
      }
      last_sequence_ = number;
      return {};
    case Tag::CompactPointer: {
      std::optional<InternalKey> key;
      if (!ReadLevel(input, level) || !ReadInternalKey(input, key)) {
        return Malformed("compact pointer");
      }
      compact_pointers_.push_back(CompactPointer{.level = level, .key = std::move(*key)});
      return {};
    }
    case Tag::DeletedFile:
      if (!ReadLevel(input, level) || !ReadFileNumber(input, number)) {
        return Malformed("deleted file");
      }
      deleted_files_.insert(DeletedFile{.level = level, .number = number});
      return {};
    case Tag::NewFile: {
      std::uint64_t file_size = 0;
      std::optional<InternalKey> smallest;
      std::optional<InternalKey> largest;
      if (!ReadLevel(input, level) || !ReadFileNumber(input, number) ||
          !ReadNumber(input, file_size) || !ReadInternalKey(input, smallest) ||
          !ReadInternalKey(input, largest)) {
        return Malformed("new file");
      }
      // Build the entry first. Inside the push_back expression, GCC adds member
      // cleanup branches that only an allocation failure could take.
      NewFile entry{
          .level = level,
          .file =
              FileMetadata{
                  .number = number,
                  .file_size = file_size,
                  .smallest = std::move(*smallest),
                  .largest = std::move(*largest),
              },
      };
      new_files_.push_back(std::move(entry));
      return {};
    }
  }
  return std::unexpected(Error::Corruption("version edit has an unknown tag"));
}

}  // namespace modern_leveldb
